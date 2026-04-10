use std::{
    collections::VecDeque,
    fs,
    net::SocketAddr,
    path::PathBuf,
    sync::{
        Arc,
        atomic::{AtomicU64, Ordering},
    },
    time::Duration,
};

use anyhow::{Context, Result, anyhow};
use axum::{
    Json, Router,
    extract::State,
    http::StatusCode,
    response::IntoResponse,
    routing::{get, post},
};
use directories::ProjectDirs;
use tokio::{
    net::TcpListener,
    sync::{Mutex, mpsc},
};
use tracing::{debug, info, warn};

use crate::{
    approvals::{ApprovalRequest, ApprovalStore},
    compat,
    device::{DeviceConfig, DeviceEvent, DeviceManager, SessionFactory, UnixSerialFactory},
    model::{
        AdminErrorResponse, AdminRpcResponse, AgentStatusResponse, LocalHookEvent, PersistedState,
        RpcRequest, Snapshot,
    },
    normalizer::{apply_notification_status, materially_equal, normalize},
    text_sanitize::{
        sanitize_approval_summary, sanitize_permission_mode, sanitize_tool_name,
        sanitize_transport_id,
    },
};

const RING_BUFFER_CAPACITY: usize = 64;
const PRE_TOOL_COALESCE_MS: u64 = 300;
const INACTIVITY_TIMEOUT_SECS: u64 = 300;
const INACTIVITY_CHECK_INTERVAL_SECS: u64 = 60;

#[derive(Debug, Clone)]
pub struct Config {
    pub admin_addr: SocketAddr,
    pub admin_addr_raw: String,
    pub state_path: PathBuf,
    pub device: DeviceConfig,
}

impl Config {
    pub fn from_env() -> Result<Self> {
        let admin_addr_raw = compat::admin_addr();
        let admin_addr = admin_addr_raw
            .parse()
            .with_context(|| format!("invalid admin addr: {admin_addr_raw}"))?;

        let state_dir = if let Some(state_dir) = compat::state_dir() {
            PathBuf::from(state_dir)
        } else {
            let project_dirs = ProjectDirs::from("local", "esp32dash", "esp32dash")
                .ok_or_else(|| anyhow!("failed to resolve project directories"))?;
            project_dirs.data_dir().to_path_buf()
        };
        fs::create_dir_all(&state_dir).context("failed to create state directory")?;

        Ok(Self {
            admin_addr,
            admin_addr_raw,
            state_path: state_dir.join("state.json"),
            device: DeviceConfig {
                preferred_port: compat::serial_port(),
                baud: compat::serial_baud(),
            },
        })
    }
}

#[derive(Debug)]
struct PendingToolEvent {
    generation: u64,
    event: LocalHookEvent,
}

#[derive(Debug)]
struct AgentState {
    seq: u64,
    snapshot: Snapshot,
    history: VecDeque<Snapshot>,
    pending_tool: Option<PendingToolEvent>,
    last_event_ts: std::time::Instant,
}

impl AgentState {
    fn new(initial: Snapshot) -> Self {
        let seq = initial.seq;
        Self {
            seq,
            snapshot: initial,
            history: VecDeque::with_capacity(RING_BUFFER_CAPACITY),
            pending_tool: None,
            last_event_ts: std::time::Instant::now(),
        }
    }
}

#[derive(Clone)]
pub struct AppState {
    config: Config,
    state: Arc<Mutex<AgentState>>,
    pending_generation: Arc<AtomicU64>,
    device_manager: DeviceManager,
    pub approvals: ApprovalStore,
}

pub async fn run(config: Config) -> Result<()> {
    let app_state = build_app_state(config.clone(), Arc::new(UnixSerialFactory));

    // Spawn inactivity timer
    let inactivity_state = Arc::new(app_state.clone());
    tokio::spawn(async move {
        let mut interval =
            tokio::time::interval(Duration::from_secs(INACTIVITY_CHECK_INTERVAL_SECS));
        interval.tick().await; // skip immediate first tick
        loop {
            interval.tick().await;
            let guard = inactivity_state.state.lock().await;
            let elapsed = guard.last_event_ts.elapsed();
            let is_active = !matches!(guard.snapshot.status.as_str(), "ended" | "unknown");
            drop(guard);

            if is_active && elapsed >= Duration::from_secs(INACTIVITY_TIMEOUT_SECS) {
                info!(
                    "inactivity timeout ({}s), transitioning to sleep",
                    elapsed.as_secs()
                );
                let sleep_event = LocalHookEvent {
                    session_id: String::new(),
                    cwd: String::new(),
                    hook_event_name: "Stop".into(),
                    message: Some("Inactivity timeout".into()),
                    prompt_preview: None,
                    tool_name: None,
                    tool_use_id: None,
                    permission_mode: "default".into(),
                    recv_ts: now_epoch(),
                };
                inactivity_state.ingest(sleep_event).await;
            }
        }
    });

    let router = build_router(app_state);

    let listener = TcpListener::bind(config.admin_addr)
        .await
        .with_context(|| format!("failed to bind admin addr {}", config.admin_addr_raw))?;

    info!(
        "esp32dash admin endpoint listening on {}",
        config.admin_addr_raw
    );

    tokio::select! {
        res = axum::serve(listener, router) => {
            res.context("admin server failed")?;
        }
        _ = tokio::signal::ctrl_c() => {
            info!("received ctrl-c, shutting down");
        }
    }

    Ok(())
}

impl AppState {
    pub async fn ingest(self: &Arc<Self>, event: LocalHookEvent) {
        self.maybe_dismiss_approvals_for_event(&event).await;
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

    async fn maybe_dismiss_approvals_for_event(self: &Arc<Self>, event: &LocalHookEvent) {
        if !event_clears_pending_approval(event) {
            return;
        }

        let dismissed = self.approvals.dismiss_matching(event).await;
        for approval_id in dismissed {
            info!(
                approval_id,
                session_id = event.session_id,
                hook_event = event.hook_event_name,
                "host cleared pending approval, dismissing device overlay"
            );
            if let Err(err) = self.device_manager.send_protocol_event(
                "claude.approval.dismiss",
                serde_json::json!({ "id": approval_id }),
            ) {
                warn!(
                    session_id = event.session_id,
                    hook_event = event.hook_event_name,
                    error = %err,
                    "failed to push approval dismiss event to device"
                );
            }
        }
    }

    async fn process_event(self: &Arc<Self>, event: LocalHookEvent) {
        let persist = {
            let mut guard = self.state.lock().await;
            guard.last_event_ts = std::time::Instant::now();
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

            PersistedState {
                seq: guard.seq,
                snapshot: next,
            }
        };

        if let Err(err) = persist_state(&self.config.state_path, &persist) {
            warn!("failed to persist state: {err:#}");
        }

        self.device_manager.send_snapshot(persist.snapshot);
    }

    async fn snapshot(&self) -> Snapshot {
        self.state.lock().await.snapshot.clone()
    }

    fn serial_status(&self) -> crate::model::SerialConnectionStatus {
        self.device_manager.status()
    }
}

fn event_clears_pending_approval(event: &LocalHookEvent) -> bool {
    !event.session_id.is_empty()
        && event.hook_event_name != "PermissionRequest"
        && event.hook_event_name != "Notification"
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
    Json(AgentStatusResponse {
        admin_addr: state.config.admin_addr_raw.clone(),
        current_seq: snapshot.seq,
        serial: state.serial_status(),
        snapshot,
    })
}

async fn get_snapshot(State(state): State<AppState>) -> impl IntoResponse {
    Json(state.snapshot().await)
}

async fn post_device_rpc(
    State(state): State<AppState>,
    Json(request): Json<RpcRequest>,
) -> Result<Json<AdminRpcResponse>, (StatusCode, Json<AdminErrorResponse>)> {
    match state.device_manager.rpc(request).await {
        Ok(result) => Ok(Json(AdminRpcResponse {
            ok: true,
            result: Some(result),
            error: None,
        })),
        Err(err) => Err((
            StatusCode::SERVICE_UNAVAILABLE,
            Json(AdminErrorResponse {
                ok: false,
                error: err.to_string(),
            }),
        )),
    }
}

use serde::Deserialize;

#[derive(Debug, Deserialize)]
struct SubmitApprovalBody {
    id: String,
    tool_name: String,
    tool_input_summary: String,
    #[serde(default = "default_permission_mode")]
    permission_mode: String,
    #[serde(default)]
    session_id: String,
    #[serde(default)]
    tool_use_id: Option<String>,
}

fn default_permission_mode() -> String {
    "default".into()
}

async fn post_approval(
    State(state): State<AppState>,
    Json(body): Json<SubmitApprovalBody>,
) -> (StatusCode, Json<serde_json::Value>) {
    let tool_name = sanitize_tool_name(&body.tool_name).unwrap_or_else(|| "unknown".into());
    let tool_input_summary = sanitize_approval_summary(&body.tool_input_summary);
    let permission_mode = {
        let mode = sanitize_permission_mode(&body.permission_mode);
        if mode.is_empty() {
            "default".into()
        } else {
            mode
        }
    };
    let transport_id = sanitize_transport_id(&body.id);
    let request = ApprovalRequest {
        tool_name: tool_name.clone(),
        tool_input_summary: tool_input_summary.clone(),
        permission_mode,
        session_id: body.session_id.trim().to_string(),
        tool_use_id: body
            .tool_use_id
            .map(|value| value.trim().to_string())
            .filter(|value| !value.is_empty()),
    };
    let id = state.approvals.submit(body.id, request.clone()).await;
    info!(
        approval_id = id,
        tool = tool_name,
        "approval submitted, forwarding to device"
    );

    if let Err(err) = state.device_manager.send_protocol_event(
        "claude.approval.request",
        serde_json::json!({
            "id": transport_id,
            "tool_name": request.tool_name,
            "description": request.tool_input_summary,
        }),
    ) {
        warn!(approval_id = id, error = %err, "failed to forward approval request to device");
    }

    (
        StatusCode::CREATED,
        Json(serde_json::json!({ "ok": true, "id": id })),
    )
}

async fn get_approval(
    State(state): State<AppState>,
    axum::extract::Path(id): axum::extract::Path<String>,
) -> (StatusCode, Json<serde_json::Value>) {
    match state.approvals.status(&id).await {
        Some(status) => (
            StatusCode::OK,
            Json(serde_json::json!({
                "ok": true,
                "approval": status,
            })),
        ),
        None => (
            StatusCode::NOT_FOUND,
            Json(serde_json::json!({
                "ok": false,
                "error": "approval not found",
            })),
        ),
    }
}

fn load_state(path: &PathBuf) -> Option<PersistedState> {
    let contents = match fs::read_to_string(path) {
        Ok(c) => c,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return None,
        Err(e) => {
            warn!("failed to read state file {}: {e}", path.display());
            return None;
        }
    };
    match serde_json::from_str(&contents) {
        Ok(state) => Some(state),
        Err(e) => {
            warn!(
                "corrupted state file {}, starting fresh: {e}",
                path.display()
            );
            None
        }
    }
}

fn persist_state(path: &PathBuf, state: &PersistedState) -> Result<()> {
    use std::io::Write;

    let json = serde_json::to_string_pretty(state)?;
    let tmp_path = path.with_extension("json.tmp");

    let mut file = fs::File::create(&tmp_path)
        .with_context(|| format!("failed to create temp state file {}", tmp_path.display()))?;
    file.write_all(json.as_bytes())?;
    file.sync_all()?;
    fs::rename(&tmp_path, path).with_context(|| {
        format!(
            "failed to rename {} -> {}",
            tmp_path.display(),
            path.display()
        )
    })?;
    Ok(())
}

fn build_app_state(config: Config, factory: Arc<dyn SessionFactory>) -> AppState {
    let initial_snapshot = load_state(&config.state_path)
        .map(|persisted| persisted.snapshot)
        .unwrap_or_else(|| Snapshot::empty(now_epoch()));
    let approvals = ApprovalStore::new();
    let (device_event_tx, mut device_event_rx) = mpsc::unbounded_channel();
    let device_manager = DeviceManager::start(
        config.device.clone(),
        factory,
        Some(initial_snapshot.clone()),
        device_event_tx,
    );

    let approvals_for_device = approvals.clone();
    tokio::spawn(async move {
        while let Some(event) = device_event_rx.recv().await {
            match event {
                DeviceEvent::ApprovalResolved { id, decision } => {
                    if approvals_for_device.resolve(&id, decision).await {
                        info!(approval_id = id, ?decision, "device resolved approval");
                    } else {
                        warn!(
                            approval_id = id,
                            ?decision,
                            "dropping approval result for unknown request"
                        );
                    }
                }
            }
        }
    });

    AppState {
        config,
        state: Arc::new(Mutex::new(AgentState::new(initial_snapshot))),
        pending_generation: Arc::new(AtomicU64::new(0)),
        device_manager,
        approvals,
    }
}

fn build_router(app_state: AppState) -> Router {
    Router::new()
        .route("/v1/claude/events", post(post_event))
        .route("/v1/events", post(post_event))
        .route("/v1/agent/status", get(get_status))
        .route("/v1/status", get(get_status))
        .route("/v1/snapshot", get(get_snapshot))
        .route("/v1/device/rpc", post(post_device_rpc))
        .route("/v1/claude/approvals", post(post_approval))
        .route("/v1/claude/approvals/{id}", get(get_approval))
        .with_state(app_state)
}

fn now_epoch() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs()
}

#[cfg(test)]
mod tests {
    use std::{
        sync::{Arc, Mutex},
        time::SystemTime,
    };

    use anyhow::Result;
    use reqwest::Client;

    use super::*;
    use crate::{
        device::DeviceSession,
        model::{Attention, WireFrame},
    };

    #[derive(Clone)]
    struct FakeFactory {
        writes: Arc<Mutex<Vec<WireFrame>>>,
    }

    impl SessionFactory for FakeFactory {
        fn connect(&self, _port: &str, _baud: u32) -> Result<Box<dyn DeviceSession>> {
            Ok(Box::new(FakeSession {
                writes: self.writes.clone(),
                sent_hello: false,
            }))
        }
    }

    struct FakeSession {
        writes: Arc<Mutex<Vec<WireFrame>>>,
        sent_hello: bool,
    }

    impl DeviceSession for FakeSession {
        fn read_frame(&mut self, timeout: Duration) -> Result<Option<WireFrame>> {
            if !self.sent_hello {
                self.sent_hello = true;
                return Ok(Some(WireFrame::Hello {
                    protocol_version: 1,
                    device_id: "esp32-dashboard".into(),
                    product: "waveshare".into(),
                    capabilities: vec!["claude.update".into(), "config.get".into()],
                }));
            }
            std::thread::sleep(timeout.min(Duration::from_millis(5)));
            Ok(None)
        }

        fn write_frame(&mut self, frame: &WireFrame) -> Result<()> {
            self.writes
                .lock()
                .expect("writes mutex poisoned")
                .push(frame.clone());
            Ok(())
        }
    }

    #[tokio::test]
    async fn status_endpoint_returns_serial_and_snapshot_state() -> Result<()> {
        let unique = SystemTime::now()
            .duration_since(SystemTime::UNIX_EPOCH)?
            .as_nanos();
        let state_dir = std::env::temp_dir().join(format!("esp32dash-status-test-{unique}"));
        fs::create_dir_all(&state_dir)?;
        let writes = Arc::new(Mutex::new(Vec::new()));

        let config = Config {
            admin_addr: "127.0.0.1:0".parse()?,
            admin_addr_raw: "127.0.0.1:0".into(),
            state_path: state_dir.join("state.json"),
            device: DeviceConfig {
                preferred_port: Some("/dev/fake".into()),
                baud: 115_200,
            },
        };

        let app_state = build_app_state(
            config,
            Arc::new(FakeFactory {
                writes: writes.clone(),
            }),
        );
        let router = build_router(app_state);
        let listener = TcpListener::bind("127.0.0.1:0").await?;
        let addr = listener.local_addr()?;
        let server = tokio::spawn(async move { axum::serve(listener, router).await });

        tokio::time::sleep(Duration::from_millis(50)).await;

        let body = Client::new()
            .get(format!("http://{addr}/v1/agent/status"))
            .send()
            .await?
            .error_for_status()?
            .text()
            .await?;

        let status: AgentStatusResponse = serde_json::from_str(&body)?;
        assert_eq!(status.snapshot.title, "No data yet");
        assert!(status.serial.connected);
        assert_eq!(status.serial.device_id.as_deref(), Some("esp32-dashboard"));

        server.abort();
        Ok(())
    }

    #[tokio::test]
    async fn ingest_event_pushes_snapshot_to_serial_worker() -> Result<()> {
        let unique = SystemTime::now()
            .duration_since(SystemTime::UNIX_EPOCH)?
            .as_nanos();
        let state_dir = std::env::temp_dir().join(format!("esp32dash-ingest-test-{unique}"));
        fs::create_dir_all(&state_dir)?;
        let writes = Arc::new(Mutex::new(Vec::new()));

        let config = Config {
            admin_addr: "127.0.0.1:0".parse()?,
            admin_addr_raw: "127.0.0.1:0".into(),
            state_path: state_dir.join("state.json"),
            device: DeviceConfig {
                preferred_port: Some("/dev/fake".into()),
                baud: 115_200,
            },
        };

        let app_state = build_app_state(
            config,
            Arc::new(FakeFactory {
                writes: writes.clone(),
            }),
        );
        let router = build_router(app_state);
        let listener = TcpListener::bind("127.0.0.1:0").await?;
        let addr = listener.local_addr()?;
        let server = tokio::spawn(async move { axum::serve(listener, router).await });

        Client::new()
            .post(format!("http://{addr}/v1/claude/events"))
            .json(&LocalHookEvent {
                session_id: "sess-1".into(),
                cwd: "/tmp/project".into(),
                hook_event_name: "Stop".into(),
                message: Some("finished".into()),
                prompt_preview: None,
                tool_name: None,
                tool_use_id: None,
                permission_mode: "default".into(),
                recv_ts: 123,
            })
            .send()
            .await?
            .error_for_status()?;

        tokio::time::sleep(Duration::from_millis(100)).await;

        let writes = writes.lock().expect("writes mutex poisoned");
        assert!(!writes.is_empty());
        let last = writes
            .last()
            .expect("serial worker should have written a frame");
        match last {
            WireFrame::Event { method, payload } => {
                assert_eq!(method, "claude.update");
                assert_eq!(payload["status"], "unknown");
                assert_eq!(
                    payload["attention"],
                    serde_json::to_value(Attention::Medium)?
                );
            }
            other => panic!("unexpected frame written to serial worker: {other:?}"),
        }

        server.abort();
        Ok(())
    }

    #[tokio::test]
    async fn host_follow_up_event_dismisses_pending_approval_on_device() -> Result<()> {
        let unique = SystemTime::now()
            .duration_since(SystemTime::UNIX_EPOCH)?
            .as_nanos();
        let state_dir = std::env::temp_dir().join(format!("esp32dash-approval-test-{unique}"));
        fs::create_dir_all(&state_dir)?;
        let writes = Arc::new(Mutex::new(Vec::new()));

        let config = Config {
            admin_addr: "127.0.0.1:0".parse()?,
            admin_addr_raw: "127.0.0.1:0".into(),
            state_path: state_dir.join("state.json"),
            device: DeviceConfig {
                preferred_port: Some("/dev/fake".into()),
                baud: 115_200,
            },
        };

        let app_state = build_app_state(
            config,
            Arc::new(FakeFactory {
                writes: writes.clone(),
            }),
        );
        let router = build_router(app_state);
        let listener = TcpListener::bind("127.0.0.1:0").await?;
        let addr = listener.local_addr()?;
        let server = tokio::spawn(async move { axum::serve(listener, router).await });

        Client::new()
            .post(format!("http://{addr}/v1/claude/approvals"))
            .json(&serde_json::json!({
                "id": "approval-1",
                "tool_name": "Bash",
                "tool_input_summary": "rm -rf /tmp/test",
                "permission_mode": "default",
                "session_id": "sess-1",
                "tool_use_id": "tool-1",
            }))
            .send()
            .await?
            .error_for_status()?;

        Client::new()
            .post(format!("http://{addr}/v1/claude/events"))
            .json(&LocalHookEvent {
                session_id: "sess-1".into(),
                cwd: "/tmp/project".into(),
                hook_event_name: "PreToolUse".into(),
                message: None,
                prompt_preview: None,
                tool_name: Some("Bash".into()),
                tool_use_id: Some("tool-1".into()),
                permission_mode: "default".into(),
                recv_ts: 124,
            })
            .send()
            .await?
            .error_for_status()?;

        tokio::time::sleep(Duration::from_millis(100)).await;

        let writes = writes.lock().expect("writes mutex poisoned");
        assert!(writes.iter().any(|frame| {
            matches!(
                frame,
                WireFrame::Event { method, payload }
                    if method == "claude.approval.request"
                        && payload["id"] == "approval-1"
            )
        }));
        assert!(writes.iter().any(|frame| {
            matches!(
                frame,
                WireFrame::Event { method, payload }
                    if method == "claude.approval.dismiss"
                        && payload["id"] == "approval-1"
            )
        }));

        server.abort();
        Ok(())
    }
}
