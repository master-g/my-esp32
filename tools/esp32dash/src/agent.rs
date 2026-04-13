use std::{
    collections::{BTreeMap, VecDeque},
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
        AdminErrorResponse, AdminRpcResponse, AgentStatusResponse, LocalHookEvent,
        PersistedSessionState, PersistedState, RpcRequest, RunStatus, Snapshot,
    },
    normalizer::{apply_notification_status, materially_equal, normalize},
    text_sanitize::{
        sanitize_approval_summary, sanitize_permission_mode, sanitize_tool_name,
        sanitize_transport_id,
    },
};

const RING_BUFFER_CAPACITY: usize = 64;
const PRE_TOOL_COALESCE_MS: u64 = 300;
const SESSION_RECONCILE_INTERVAL_SECS: u64 = 5;
const HEARTBEAT_INTERVAL_SECS: u64 = 10;
const PIDLESS_ACTIVE_SESSION_TIMEOUT_SECS: u64 = 900;
const IDLE_SESSION_RETENTION_SECS: u64 = 1800;
const ENDED_SESSION_RETENTION_SECS: u64 = 300;
const MAX_TRACKED_SESSIONS: usize = 16;

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
    sessions: BTreeMap<String, PersistedSessionState>,
    last_heartbeat_ts: u64,
}

impl AgentState {
    fn new(initial: Snapshot, sessions: BTreeMap<String, PersistedSessionState>) -> Self {
        let seq = initial.seq;
        Self {
            seq,
            snapshot: initial,
            history: VecDeque::with_capacity(RING_BUFFER_CAPACITY),
            pending_tool: None,
            sessions,
            last_heartbeat_ts: 0,
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

    let reconcile_state = Arc::new(app_state.clone());
    tokio::spawn(async move {
        let mut interval =
            tokio::time::interval(Duration::from_secs(SESSION_RECONCILE_INTERVAL_SECS));
        interval.tick().await; // skip immediate first tick
        loop {
            interval.tick().await;
            reconcile_state.reconcile_sessions().await;
        }
    });

    let router = build_router(app_state);

    let listener = TcpListener::bind(config.admin_addr)
        .await
        .with_context(|| format!("failed to bind admin addr {}", config.admin_addr_raw))?;

    info!("esp32dash admin endpoint listening on {}", config.admin_addr_raw);

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
            "PostToolUse" | "PostToolUseFailure" => self.handle_post_tool(event).await,
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
        let (persist, snapshot_to_send) = {
            let mut guard = self.state.lock().await;
            apply_event_to_sessions(&mut guard.sessions, &event);
            finalize_guard_state(&mut guard, event.recv_ts, true)
        };

        if let Some(persist) = persist {
            if let Err(err) = persist_state(&self.config.state_path, &persist) {
                warn!("failed to persist state: {err:#}");
            }
        }

        if let Some(snapshot) = snapshot_to_send {
            self.device_manager.send_snapshot(snapshot);
        }
    }

    async fn reconcile_sessions(self: &Arc<Self>) {
        let now = now_epoch();
        let (persist, snapshot_to_send, heartbeat_snapshot) = {
            let mut guard = self.state.lock().await;
            let sessions_changed = reconcile_session_map(&mut guard.sessions, now);
            let (persist, snapshot_to_send) =
                finalize_guard_state(&mut guard, now, sessions_changed);
            let heartbeat_snapshot = if !guard.sessions.is_empty()
                && now.saturating_sub(guard.last_heartbeat_ts) >= HEARTBEAT_INTERVAL_SECS
            {
                guard.last_heartbeat_ts = now;
                Some(guard.snapshot.clone())
            } else {
                None
            };
            (persist, snapshot_to_send, heartbeat_snapshot)
        };

        if let Some(persist) = persist {
            if let Err(err) = persist_state(&self.config.state_path, &persist) {
                warn!("failed to persist state: {err:#}");
            }
        }

        if let Some(snapshot) = snapshot_to_send {
            self.device_manager.send_snapshot(snapshot);
        }

        if let Some(snapshot) = heartbeat_snapshot {
            if let Err(err) = self.device_manager.send_protocol_event(
                "claude.heartbeat",
                serde_json::json!({
                    "ts": now,
                    "seq": snapshot.seq,
                    "session_id": snapshot.session_id,
                    "status": snapshot.status,
                }),
            ) {
                warn!(error = %err, "failed to send claude heartbeat to device");
            }
        }
    }

    async fn snapshot(&self) -> Snapshot {
        self.state.lock().await.snapshot.clone()
    }

    fn serial_status(&self) -> crate::model::SerialConnectionStatus {
        self.device_manager.status()
    }
}

fn finalize_guard_state(
    guard: &mut AgentState,
    fallback_ts: u64,
    sessions_changed: bool,
) -> (Option<PersistedState>, Option<Snapshot>) {
    let next_snapshot = select_display_snapshot(&guard.sessions, fallback_ts);
    let mut snapshot_to_send = None;
    let snapshot_changed = !materially_equal(&next_snapshot, &guard.snapshot);

    if snapshot_changed {
        guard.seq += 1;
        let mut published = next_snapshot;
        published.seq = guard.seq;
        guard.snapshot = published.clone();
        guard.history.push_back(published.clone());
        while guard.history.len() > RING_BUFFER_CAPACITY {
            guard.history.pop_front();
        }
        snapshot_to_send = Some(published);
    } else {
        guard.snapshot.ts = next_snapshot.ts;
        debug!("coalesced duplicate visible claude snapshot");
    }

    let persist = if sessions_changed || snapshot_changed {
        Some(PersistedState {
            seq: guard.seq,
            snapshot: guard.snapshot.clone(),
            sessions: guard.sessions.values().cloned().collect(),
        })
    } else {
        None
    };

    (persist, snapshot_to_send)
}

fn apply_event_to_sessions(
    sessions: &mut BTreeMap<String, PersistedSessionState>,
    event: &LocalHookEvent,
) {
    let key = session_key_for(event);
    let entry = sessions.entry(key.clone()).or_insert_with(|| PersistedSessionState {
        key: key.clone(),
        snapshot: Snapshot::empty(event.recv_ts),
        cwd: event.cwd.clone(),
        last_activity_ts: event.recv_ts,
        claude_pid: event.claude_pid,
    });

    let current = entry.snapshot.clone();
    let mut next = normalize(event, &current);
    if event.hook_event_name == "Notification" {
        next = apply_notification_status(next, &current, event);
    }
    if next.session_id.is_empty() {
        next.session_id = key.clone();
    }

    entry.snapshot = next;
    entry.cwd = if event.cwd.trim().is_empty() {
        entry.cwd.clone()
    } else {
        event.cwd.clone()
    };
    entry.last_activity_ts = event.recv_ts;
    if let Some(pid) = event.claude_pid {
        entry.claude_pid = Some(pid);
    }
    if event.hook_event_name == "SessionEnd" {
        entry.claude_pid = None;
    }
}

fn reconcile_session_map(sessions: &mut BTreeMap<String, PersistedSessionState>, now: u64) -> bool {
    let mut changed = false;

    for session in sessions.values_mut() {
        if session_liveness_lost(session, now) {
            mark_session_idle(session, now);
            changed = true;
        }
    }

    let before = sessions.len();
    sessions.retain(|_, session| !should_prune_session(session, now));
    changed |= sessions.len() != before;

    changed | prune_excess_sessions(sessions)
}

fn prune_excess_sessions(sessions: &mut BTreeMap<String, PersistedSessionState>) -> bool {
    if sessions.len() <= MAX_TRACKED_SESSIONS {
        return false;
    }

    let mut candidates: Vec<(String, u8, u64)> = sessions
        .values()
        .map(|session| {
            (
                session.key.clone(),
                RunStatus::from_str(&session.snapshot.status).display_priority(),
                session.last_activity_ts,
            )
        })
        .collect();
    candidates.sort_by_key(|(_, priority, last_activity_ts)| (*priority, *last_activity_ts));

    let remove_count = sessions.len().saturating_sub(MAX_TRACKED_SESSIONS);
    for (key, _, _) in candidates.into_iter().take(remove_count) {
        sessions.remove(&key);
    }
    true
}

fn select_display_snapshot(
    sessions: &BTreeMap<String, PersistedSessionState>,
    fallback_ts: u64,
) -> Snapshot {
    sessions
        .values()
        .max_by_key(|session| {
            (
                RunStatus::from_str(&session.snapshot.status).display_priority(),
                u8::from(session.snapshot.unread),
                session.last_activity_ts,
            )
        })
        .map(|session| session.snapshot.clone())
        .unwrap_or_else(|| Snapshot::empty(fallback_ts))
}

fn session_key_for(event: &LocalHookEvent) -> String {
    let session_id = event.session_id.trim();
    if !session_id.is_empty() {
        return session_id.to_string();
    }
    let cwd = event.cwd.trim();
    if !cwd.is_empty() {
        return format!("cwd:{cwd}");
    }
    if let Some(pid) = event.claude_pid {
        return format!("pid:{pid}");
    }
    "__global__".into()
}

fn session_liveness_lost(session: &PersistedSessionState, now: u64) -> bool {
    let status = RunStatus::from_str(&session.snapshot.status);
    if !status.is_active() {
        return false;
    }

    if let Some(pid) = session.claude_pid {
        return !process_exists(pid);
    }

    now.saturating_sub(session.last_activity_ts) >= PIDLESS_ACTIVE_SESSION_TIMEOUT_SECS
}

fn should_prune_session(session: &PersistedSessionState, now: u64) -> bool {
    let age = now.saturating_sub(session.last_activity_ts);
    match RunStatus::from_str(&session.snapshot.status) {
        RunStatus::Ended => age >= ENDED_SESSION_RETENTION_SECS,
        RunStatus::Unknown => age >= IDLE_SESSION_RETENTION_SECS,
        _ => false,
    }
}

fn mark_session_idle(session: &mut PersistedSessionState, now: u64) {
    session.last_activity_ts = now;
    session.snapshot.event = "SessionLivenessLost".into();
    session.snapshot.status = RunStatus::Unknown.as_str().into();
    session.snapshot.title = "Idle".into();
    session.snapshot.detail = if session.claude_pid.is_some() {
        "Claude session exited before sending a terminal hook".into()
    } else {
        "Claude activity timed out without a terminal hook".into()
    };
    session.snapshot.attention = crate::model::Attention::Medium;
    session.snapshot.unread = false;
    session.snapshot.ts = now;
    session.claude_pid = None;
}

fn process_exists(pid: u32) -> bool {
    let rc = unsafe { libc::kill(pid as i32, 0) };
    if rc == 0 {
        return true;
    }

    matches!(std::io::Error::last_os_error().raw_os_error(), Some(libc::EPERM))
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
    info!(approval_id = id, tool = tool_name, "approval submitted, forwarding to device");

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

    (StatusCode::CREATED, Json(serde_json::json!({ "ok": true, "id": id })))
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
            warn!("corrupted state file {}, starting fresh: {e}", path.display());
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
        format!("failed to rename {} -> {}", tmp_path.display(), path.display())
    })?;
    Ok(())
}

fn build_app_state(config: Config, factory: Arc<dyn SessionFactory>) -> AppState {
    let persisted = load_state(&config.state_path);
    let initial_snapshot = persisted
        .as_ref()
        .map(|state| state.snapshot.clone())
        .unwrap_or_else(|| Snapshot::empty(now_epoch()));
    let initial_sessions = restored_sessions(persisted.as_ref(), &initial_snapshot);
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
                DeviceEvent::ApprovalResolved {
                    id,
                    decision,
                } => {
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
        state: Arc::new(Mutex::new(AgentState::new(initial_snapshot, initial_sessions))),
        pending_generation: Arc::new(AtomicU64::new(0)),
        device_manager,
        approvals,
    }
}

fn restored_sessions(
    persisted: Option<&PersistedState>,
    initial_snapshot: &Snapshot,
) -> BTreeMap<String, PersistedSessionState> {
    let mut sessions = persisted
        .map(|state| {
            state
                .sessions
                .iter()
                .cloned()
                .map(|session| (session.key.clone(), session))
                .collect::<BTreeMap<_, _>>()
        })
        .unwrap_or_default();

    if sessions.is_empty() && initial_snapshot.seq > 0 {
        let key = if initial_snapshot.session_id.trim().is_empty() {
            "__restored__".to_string()
        } else {
            initial_snapshot.session_id.clone()
        };
        sessions.insert(
            key.clone(),
            PersistedSessionState {
                key,
                snapshot: initial_snapshot.clone(),
                cwd: String::new(),
                last_activity_ts: initial_snapshot.ts,
                claude_pid: None,
            },
        );
    }

    sessions
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
    std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap_or_default().as_secs()
}

#[cfg(test)]
mod tests {
    use std::{
        collections::BTreeMap,
        process,
        sync::{Arc, Mutex},
        time::SystemTime,
    };

    use anyhow::Result;
    use reqwest::Client;

    use super::*;
    use crate::{
        device::DeviceSession,
        model::{Attention, PersistedSessionState, WireFrame},
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
            self.writes.lock().expect("writes mutex poisoned").push(frame.clone());
            Ok(())
        }
    }

    #[tokio::test]
    async fn status_endpoint_returns_serial_and_snapshot_state() -> Result<()> {
        let unique = SystemTime::now().duration_since(SystemTime::UNIX_EPOCH)?.as_nanos();
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
        let unique = SystemTime::now().duration_since(SystemTime::UNIX_EPOCH)?.as_nanos();
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
                claude_pid: None,
            })
            .send()
            .await?
            .error_for_status()?;

        tokio::time::sleep(Duration::from_millis(100)).await;

        let writes = writes.lock().expect("writes mutex poisoned");
        assert!(!writes.is_empty());
        let last = writes.last().expect("serial worker should have written a frame");
        match last {
            WireFrame::Event {
                method,
                payload,
            } => {
                assert_eq!(method, "claude.update");
                assert_eq!(payload["status"], "unknown");
                assert_eq!(payload["attention"], serde_json::to_value(Attention::Medium)?);
            }
            other => panic!("unexpected frame written to serial worker: {other:?}"),
        }

        server.abort();
        Ok(())
    }

    #[tokio::test]
    async fn host_follow_up_event_dismisses_pending_approval_on_device() -> Result<()> {
        let unique = SystemTime::now().duration_since(SystemTime::UNIX_EPOCH)?.as_nanos();
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
                claude_pid: None,
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

    #[test]
    fn display_snapshot_prefers_waiting_session_over_newer_idle() {
        let mut sessions = BTreeMap::new();
        let mut idle = Snapshot::empty(10);
        idle.session_id = "idle".into();
        idle.title = "Idle".into();
        idle.detail = "done".into();

        let mut waiting = Snapshot::empty(9);
        waiting.session_id = "waiting".into();
        waiting.status = "waiting_for_input".into();
        waiting.title = "Awaiting approval".into();
        waiting.detail = "Need approval".into();
        waiting.unread = true;
        waiting.attention = Attention::High;

        sessions.insert(
            "idle".into(),
            PersistedSessionState {
                key: "idle".into(),
                snapshot: idle,
                cwd: "/tmp/idle".into(),
                last_activity_ts: 10,
                claude_pid: None,
            },
        );
        sessions.insert(
            "waiting".into(),
            PersistedSessionState {
                key: "waiting".into(),
                snapshot: waiting,
                cwd: "/tmp/waiting".into(),
                last_activity_ts: 9,
                claude_pid: None,
            },
        );

        let selected = select_display_snapshot(&sessions, 11);
        assert_eq!(selected.session_id, "waiting");
        assert_eq!(selected.status, "waiting_for_input");
    }

    #[test]
    fn reconcile_marks_pidless_active_session_idle_after_timeout() {
        let mut sessions = BTreeMap::new();
        let mut active = Snapshot::empty(1);
        active.session_id = "sess-1".into();
        active.status = "running_tool".into();
        active.title = "Running tool".into();
        active.detail = "Bash".into();

        sessions.insert(
            "sess-1".into(),
            PersistedSessionState {
                key: "sess-1".into(),
                snapshot: active,
                cwd: "/tmp/project".into(),
                last_activity_ts: 1,
                claude_pid: None,
            },
        );

        assert!(reconcile_session_map(&mut sessions, PIDLESS_ACTIVE_SESSION_TIMEOUT_SECS + 2));
        let session = sessions.get("sess-1").expect("session should remain tracked");
        assert_eq!(session.snapshot.status, "unknown");
        assert_eq!(session.snapshot.event, "SessionLivenessLost");
    }

    #[test]
    fn restored_sessions_backfill_legacy_snapshot() {
        let mut snapshot = Snapshot::empty(42);
        snapshot.seq = 7;
        snapshot.session_id = "sess-legacy".into();
        snapshot.status = "processing".into();

        let sessions = restored_sessions(None, &snapshot);
        let session = sessions.get("sess-legacy").expect("legacy snapshot should seed one session");
        assert_eq!(session.snapshot.status, "processing");
        assert_eq!(session.last_activity_ts, 42);
    }

    #[test]
    fn process_exists_reports_current_process_alive() {
        assert!(process_exists(process::id()));
    }
}
