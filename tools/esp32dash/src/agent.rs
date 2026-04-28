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

use anyhow::{Context, Result};
use axum::{
    Json, Router,
    extract::State,
    http::StatusCode,
    response::IntoResponse,
    routing::{get, post},
};
use tokio::{
    net::TcpListener,
    sync::{Mutex, mpsc},
};
use tracing::{debug, info, warn};

use crate::{
    approvals::{ApprovalRequest, ApprovalStore, DeviceApproval, DismissedApproval},
    compat,
    config::{AppConfig, ApprovalConfig, EmotionConfig, default_state_dir},
    device::{DeviceConfig, DeviceEvent, DeviceManager, SessionFactory, UnixSerialFactory},
    emotion::{Emotion, EmotionAnalysis, EmotionAnalyzer},
    emotion_state::EmotionState,
    model::{
        AdminErrorResponse, AdminRpcResponse, AgentStatusResponse, LocalHookEvent,
        PersistedSessionState, PersistedState, RpcRequest, RunStatus, Snapshot,
    },
    normalizer::{apply_notification_status, materially_equal, normalize},
    prompts::{DevicePrompt, DismissedPrompt, PromptStore},
    text_sanitize::{
        sanitize_approval_summary, sanitize_permission_mode, sanitize_snapshot_detail,
        sanitize_snapshot_title, sanitize_tool_name, sanitize_transport_id,
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
const EMOTION_DECAY_INTERVAL_SECS: u64 = 60;

#[derive(Debug, Clone)]
pub struct Config {
    pub admin_addr: SocketAddr,
    pub admin_addr_raw: String,
    pub state_path: PathBuf,
    pub device: DeviceConfig,
    pub emotion: EmotionConfig,
    pub approval: ApprovalConfig,
}

impl Config {
    pub fn from_env() -> Result<Self> {
        let app_config = AppConfig::load_for_agent_run()?;
        let admin_addr_raw = compat::env_value("ESP32DASH_ADMIN_ADDR")
            .or_else(|| app_config.admin_addr.clone())
            .unwrap_or_else(|| compat::DEFAULT_ADMIN_ADDR.to_string());
        let admin_addr = admin_addr_raw
            .parse()
            .with_context(|| format!("invalid admin addr: {admin_addr_raw}"))?;

        let state_dir = if let Some(state_dir) = compat::state_dir() {
            PathBuf::from(state_dir)
        } else if let Some(state_dir) = app_config.state_dir.clone() {
            state_dir
        } else {
            default_state_dir()?
        };
        fs::create_dir_all(&state_dir).context("failed to create state directory")?;

        Ok(Self {
            admin_addr,
            admin_addr_raw,
            state_path: state_dir.join("state.json"),
            device: DeviceConfig {
                preferred_port: compat::serial_port().or_else(|| app_config.serial_port.clone()),
                baud: resolved_serial_baud(compat::serial_baud_env(), app_config.serial_baud),
                rpc_timeout_approval: Duration::from_secs(app_config.approval.timeout_secs + 10),
            },
            emotion: app_config.emotion.clone(),
            approval: app_config.approval.clone(),
        })
    }
}

fn resolved_serial_baud(env_baud: Option<u32>, config_baud: Option<u32>) -> u32 {
    env_baud.or(config_baud).unwrap_or(compat::DEFAULT_SERIAL_BAUD)
}

#[derive(Debug)]
struct PendingToolEvent {
    generation: u64,
    event: LocalHookEvent,
}

#[derive(Debug, Clone, Default)]
struct SessionEmotionEntry {
    generation: u64,
    state: EmotionState,
}

#[derive(Debug)]
struct AgentState {
    seq: u64,
    snapshot: Snapshot,
    history: VecDeque<Snapshot>,
    pending_tool: Option<PendingToolEvent>,
    sessions: BTreeMap<String, PersistedSessionState>,
    emotions: BTreeMap<String, SessionEmotionEntry>,
    last_heartbeat_ts: u64,
    has_pending_prompt: bool,
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
            emotions: BTreeMap::new(),
            last_heartbeat_ts: 0,
            has_pending_prompt: false,
        }
    }
}

#[derive(Clone)]
pub struct AppState {
    config: Config,
    state: Arc<Mutex<AgentState>>,
    pending_generation: Arc<AtomicU64>,
    emotion_generation: Arc<AtomicU64>,
    device_manager: DeviceManager,
    emotion_analyzer: Option<EmotionAnalyzer>,
    pub approvals: ApprovalStore,
    pub prompts: PromptStore,
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

    let decay_state = Arc::new(app_state.clone());
    tokio::spawn(async move {
        let mut interval = tokio::time::interval(Duration::from_secs(EMOTION_DECAY_INTERVAL_SECS));
        interval.tick().await; // skip immediate first tick
        loop {
            interval.tick().await;
            decay_state.decay_emotions().await;
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
        self.maybe_dismiss_prompts_for_event(&event).await;
        self.maybe_dismiss_approvals_for_event(&event).await;
        match event.hook_event_name.as_str() {
            "PreToolUse" => self.handle_pre_tool(event).await,
            "PostToolUse" | "PostToolUseFailure" => self.handle_post_tool(event).await,
            _ => {
                self.flush_pending_tool().await;
                self.process_event(event).await;
            }
        }
        self.sync_device_approvals().await;
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
            self.sync_device_approvals().await;
        }
    }

    async fn flush_pending_tool(self: &Arc<Self>) {
        let pending = {
            let mut guard = self.state.lock().await;
            guard.pending_tool.take().map(|pending| pending.event)
        };

        if let Some(event) = pending {
            self.process_event(event).await;
            self.sync_device_approvals().await;
        }
    }

    async fn maybe_dismiss_approvals_for_event(self: &Arc<Self>, event: &LocalHookEvent) {
        if !event_clears_pending_approval(event) {
            return;
        }

        let dismissed = self.approvals.dismiss_matching(event).await;
        for approval in dismissed {
            info!(
                approval_id = approval.id,
                session_id = event.session_id,
                hook_event = event.hook_event_name,
                "host cleared pending approval"
            );
            if !approval.was_visible_on_device {
                continue;
            }

            if let Err(err) = self.send_device_dismiss(&approval) {
                warn!(
                    approval_id = approval.id,
                    session_id = event.session_id,
                    hook_event = event.hook_event_name,
                    error = %err,
                    "failed to push approval dismiss event to device"
                );
            }
        }
    }

    async fn maybe_dismiss_prompts_for_event(self: &Arc<Self>, event: &LocalHookEvent) {
        if !event_clears_pending_prompt(event) {
            return;
        }

        let dismissed = self.prompts.dismiss_matching(event).await;
        for prompt in dismissed {
            info!(
                prompt_id = prompt.id,
                session_id = event.session_id,
                hook_event = event.hook_event_name,
                "host cleared pending prompt"
            );
            if !prompt.was_visible_on_device {
                continue;
            }

            if let Err(err) = self.send_device_prompt_dismiss(&prompt) {
                warn!(
                    prompt_id = prompt.id,
                    session_id = event.session_id,
                    hook_event = event.hook_event_name,
                    error = %err,
                    "failed to push prompt dismiss event to device"
                );
            }
        }
    }

    async fn process_event(self: &Arc<Self>, event: LocalHookEvent) {
        let (persist, snapshot_to_send) = {
            let mut guard = self.state.lock().await;
            apply_event_to_sessions(&mut guard, &event);
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

        self.maybe_submit_prompt_for_event(&event).await;
        self.maybe_analyze_emotion_for_event(&event).await;
    }

    async fn maybe_submit_prompt_for_event(&self, event: &LocalHookEvent) {
        let Some(prompt) = event.waiting_prompt.clone() else {
            return;
        };

        let id = build_prompt_id(event, &prompt);
        let transport_id = sanitize_transport_id(&id);
        self.prompts
            .submit(
                id.clone(),
                transport_id,
                prompt,
                event.session_id.clone(),
                event.tool_use_id.clone(),
            )
            .await;
        info!(prompt_id = id, session_id = event.session_id, "queued waiting prompt");
    }

    async fn reconcile_sessions(self: &Arc<Self>) {
        let now = now_epoch();
        let (persist, snapshot_to_send, heartbeat_snapshot) = {
            let mut guard = self.state.lock().await;
            let sessions_changed = reconcile_session_map(&mut guard, now);
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

        self.sync_device_approvals().await;
    }

    async fn maybe_analyze_emotion_for_event(self: &Arc<Self>, event: &LocalHookEvent) {
        if event.hook_event_name != "UserPromptSubmit" {
            return;
        }
        let Some(prompt_raw) = event.prompt_raw.clone() else {
            return;
        };
        let Some(analyzer) = self.emotion_analyzer.clone() else {
            return;
        };

        let session_key = session_key_for(event);
        let generation = self.emotion_generation.fetch_add(1, Ordering::SeqCst) + 1;
        {
            let mut guard = self.state.lock().await;
            let entry = guard.emotions.entry(session_key.clone()).or_default();
            entry.generation = generation;
        }

        let app = self.clone();
        let recv_ts = event.recv_ts;
        tokio::spawn(async move {
            let analysis = match analyzer.analyze(&prompt_raw).await {
                Ok(analysis) => analysis,
                Err(err) => {
                    warn!(
                        session_key,
                        error = %err,
                        "emotion analyzer failed, falling back to neutral"
                    );
                    EmotionAnalysis::neutral()
                }
            };
            app.apply_emotion_analysis(session_key, generation, recv_ts, analysis).await;
        });
    }

    async fn apply_emotion_analysis(
        self: &Arc<Self>,
        session_key: String,
        generation: u64,
        recv_ts: u64,
        analysis: EmotionAnalysis,
    ) {
        let (persist, snapshot_to_send) = {
            let mut guard = self.state.lock().await;
            let next_emotion = {
                let Some(entry) = guard.emotions.get_mut(&session_key) else {
                    return;
                };
                if entry.generation != generation {
                    return;
                }
                entry.state.record_emotion(&analysis);
                entry.state.current_emotion()
            };

            let Some(session) = guard.sessions.get_mut(&session_key) else {
                guard.emotions.remove(&session_key);
                return;
            };
            if session.snapshot.emotion == next_emotion.as_str() {
                return;
            }

            session.snapshot.emotion = next_emotion.as_str().to_string();
            finalize_guard_state(&mut guard, recv_ts, true)
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

    async fn decay_emotions(self: &Arc<Self>) {
        let now = now_epoch();
        let (persist, snapshot_to_send) = {
            let mut guard = self.state.lock().await;
            let mut updates = Vec::new();

            for (key, entry) in guard.emotions.iter_mut() {
                let before = entry.state.current_emotion();
                entry.state.decay();
                let after = entry.state.current_emotion();
                if before != after {
                    updates.push((key.clone(), after));
                }
            }

            let mut changed = false;
            for (key, emotion) in updates {
                if let Some(session) = guard.sessions.get_mut(&key) {
                    if session.snapshot.emotion != emotion.as_str() {
                        session.snapshot.emotion = emotion.as_str().to_string();
                        changed = true;
                    }
                }
            }

            if !changed {
                return;
            }

            finalize_guard_state(&mut guard, now, true)
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

    async fn snapshot(&self) -> Snapshot {
        self.state.lock().await.snapshot.clone()
    }

    fn serial_status(&self) -> crate::model::SerialConnectionStatus {
        self.device_manager.status()
    }

    async fn sync_device_approvals(self: &Arc<Self>) {
        let has_pending_prompt = self.prompts.has_device_backlog().await;
        {
            let mut guard = self.state.lock().await;
            if guard.has_pending_prompt != has_pending_prompt {
                guard.has_pending_prompt = has_pending_prompt;
            }
        }

        if !self.serial_status().connected {
            self.approvals.note_device_disconnected().await;
            self.prompts.note_device_disconnected().await;
            return;
        }

        if let Some(expired) = self.approvals.take_expired_visible_for_device().await {
            info!(
                approval_id = expired.id,
                "approval timed out before device response, dismissing device overlay"
            );
            if let Err(err) = self.send_device_dismiss(&expired) {
                warn!(
                    approval_id = expired.id,
                    error = %err,
                    "failed to dismiss timed-out approval on device"
                );
                return;
            }
        }

        if self.approvals.has_device_backlog().await {
            if let Some(prompt) = self.prompts.requeue_visible_for_device().await {
                if let Err(err) = self.send_device_prompt_dismiss(&prompt) {
                    warn!(
                        prompt_id = prompt.id,
                        error = %err,
                        "failed to dismiss prompt while approval preempted device overlay"
                    );
                    return;
                }
            }

            let Some(approval) = self.approvals.claim_next_for_device().await else {
                return;
            };

            if let Err(err) = self.send_device_approval_request(&approval) {
                warn!(
                    approval_id = approval.id,
                    error = %err,
                    "failed to forward queued approval to device"
                );
                return;
            }

            info!(
                approval_id = approval.id,
                tool = approval.tool_name,
                "forwarded queued approval to device"
            );
            return;
        }
    }

    fn send_device_approval_request(&self, approval: &DeviceApproval) -> Result<()> {
        self.device_manager.send_protocol_event(
            "claude.approval.request",
            serde_json::json!({
                "id": approval.transport_id,
                "tool_name": approval.tool_name,
                "description": approval.tool_input_summary,
            }),
        )
    }

    fn send_device_dismiss(&self, approval: &DismissedApproval) -> Result<()> {
        self.device_manager.send_protocol_event(
            "claude.approval.dismiss",
            serde_json::json!({ "id": approval.transport_id }),
        )
    }

    fn send_device_prompt_request(&self, prompt: &DevicePrompt) -> Result<()> {
        let option_labels: Vec<String> = prompt
            .prompt
            .options
            .iter()
            .map(|o| o.label.clone())
            .collect();
        self.device_manager.send_protocol_event(
            "claude.prompt.request",
            serde_json::json!({
                "id": prompt.transport_id,
                "kind": prompt.prompt.kind.as_str(),
                "title": prompt.prompt.title,
                "question": prompt.prompt.question,
                "options_text": format_prompt_options_text(&prompt.prompt),
                "option_count": prompt.prompt.options.len(),
                "option_labels": option_labels,
            }),
        )
    }

    fn send_device_prompt_dismiss(&self, prompt: &DismissedPrompt) -> Result<()> {
        self.device_manager.send_protocol_event(
            "claude.prompt.dismiss",
            serde_json::json!({ "id": prompt.transport_id }),
        )
    }
}

fn finalize_guard_state(
    guard: &mut AgentState,
    fallback_ts: u64,
    sessions_changed: bool,
) -> (Option<PersistedState>, Option<Snapshot>) {
    let mut next_snapshot = select_display_snapshot(&guard.sessions, fallback_ts);
    next_snapshot.has_pending_prompt = guard.has_pending_prompt;
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

fn apply_event_to_sessions(guard: &mut AgentState, event: &LocalHookEvent) {
    let key = session_key_for(event);
    let entry = guard.sessions.entry(key.clone()).or_insert_with(|| PersistedSessionState {
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
    if event.hook_event_name == "SessionEnd" {
        next.emotion = Emotion::Neutral.as_str().to_string();
        guard.emotions.remove(&key);
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

fn reconcile_session_map(guard: &mut AgentState, now: u64) -> bool {
    let mut changed = false;

    let mut lost_keys = Vec::new();
    for (key, session) in guard.sessions.iter_mut() {
        if session_liveness_lost(session, now) {
            mark_session_idle(session, now);
            session.snapshot.emotion = Emotion::Neutral.as_str().to_string();
            lost_keys.push(key.clone());
            changed = true;
        }
    }
    for key in lost_keys {
        guard.emotions.remove(&key);
    }

    let before = guard.sessions.len();
    let pruned_keys: Vec<String> = guard
        .sessions
        .iter()
        .filter_map(|(key, session)| should_prune_session(session, now).then_some(key.clone()))
        .collect();
    for key in &pruned_keys {
        guard.sessions.remove(key);
        guard.emotions.remove(key);
    }
    changed |= guard.sessions.len() != before;

    changed | prune_excess_sessions(guard)
}

fn prune_excess_sessions(guard: &mut AgentState) -> bool {
    if guard.sessions.len() <= MAX_TRACKED_SESSIONS {
        return false;
    }

    let mut candidates: Vec<(String, u8, u64)> = guard
        .sessions
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

    let remove_count = guard.sessions.len().saturating_sub(MAX_TRACKED_SESSIONS);
    for (key, _, _) in candidates.into_iter().take(remove_count) {
        guard.sessions.remove(&key);
        guard.emotions.remove(&key);
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

fn event_clears_pending_prompt(event: &LocalHookEvent) -> bool {
    !event.session_id.is_empty() && event.hook_event_name != "Notification"
}

fn build_prompt_id(event: &LocalHookEvent, prompt: &crate::model::WaitingPrompt) -> String {
    let session_key = session_key_for(event);
    match prompt.kind {
        crate::model::WaitingPromptKind::AskUserQuestion => {
            let tool_part = event
                .tool_use_id
                .as_deref()
                .or(event.tool_name.as_deref())
                .unwrap_or(prompt.question.as_str());
            format!("{}-{session_key}-{tool_part}", prompt.kind.as_str())
        }
        crate::model::WaitingPromptKind::Elicitation => {
            format!("{}-{session_key}-{}-{}", prompt.kind.as_str(), prompt.title, prompt.question)
        }
    }
}

fn format_prompt_options_text(prompt: &crate::model::WaitingPrompt) -> String {
    let text = prompt
        .options
        .iter()
        .enumerate()
        .map(|(index, option)| match option.description.as_deref() {
            Some(description) if !description.is_empty() => {
                format!("{}. {} - {}", index + 1, option.label, description)
            }
            _ => format!("{}. {}", index + 1, option.label),
        })
        .collect::<Vec<_>>()
        .join("  ");
    sanitize_snapshot_detail(&text)
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
    #[serde(default)]
    permission_suggestions: Vec<serde_json::Value>,
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
        permission_suggestions: body.permission_suggestions,
    };
    let state = Arc::new(state);
    let id = state.approvals.submit(body.id, transport_id, request.clone()).await;
    info!(approval_id = id, tool = tool_name, "approval submitted");
    state.sync_device_approvals().await;

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

#[derive(Debug, Deserialize)]
struct SubmitPromptBody {
    id: String,
    kind: String,
    title: String,
    question: String,
    #[serde(default)]
    options: Vec<crate::model::WaitingPromptOption>,
    #[serde(default)]
    session_id: String,
    #[serde(default)]
    tool_use_id: Option<String>,
}

async fn post_prompt(
    State(state): State<AppState>,
    Json(body): Json<SubmitPromptBody>,
) -> (StatusCode, Json<serde_json::Value>) {
    let transport_id = sanitize_transport_id(&body.id);
    let prompt = crate::model::WaitingPrompt {
        kind: match body.kind.as_str() {
            "elicitation" => crate::model::WaitingPromptKind::Elicitation,
            _ => crate::model::WaitingPromptKind::AskUserQuestion,
        },
        title: sanitize_snapshot_title(&body.title),
        question: sanitize_snapshot_detail(&body.question),
        options: body.options.into_iter().take(4).collect(),
    };
    let id = state
        .prompts
        .submit(
            body.id,
            transport_id,
            prompt,
            body.session_id.trim().to_string(),
            body.tool_use_id.map(|v| v.trim().to_string()).filter(|v| !v.is_empty()),
        )
        .await;
    info!(prompt_id = id, "prompt submitted");
    Arc::new(state).sync_device_approvals().await;

    (StatusCode::CREATED, Json(serde_json::json!({ "ok": true, "id": id })))
}

async fn get_prompt(
    State(state): State<AppState>,
    axum::extract::Path(id): axum::extract::Path<String>,
) -> (StatusCode, Json<serde_json::Value>) {
    match state.prompts.status(&id).await {
        Some(status) => (
            StatusCode::OK,
            Json(serde_json::json!({
                "ok": true,
                "prompt": status,
            })),
        ),
        None => (
            StatusCode::NOT_FOUND,
            Json(serde_json::json!({
                "ok": false,
                "error": "prompt not found",
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

fn reset_snapshot_emotion(snapshot: &mut Snapshot) {
    snapshot.emotion = Emotion::Neutral.as_str().to_string();
}

fn reset_persisted_emotions(state: &mut PersistedState) {
    reset_snapshot_emotion(&mut state.snapshot);
    for session in &mut state.sessions {
        reset_snapshot_emotion(&mut session.snapshot);
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
    let mut persisted = load_state(&config.state_path);
    if let Some(state) = persisted.as_mut() {
        reset_persisted_emotions(state);
    }
    let initial_snapshot = persisted
        .as_ref()
        .map(|state| state.snapshot.clone())
        .unwrap_or_else(|| Snapshot::empty(now_epoch()));
    let initial_sessions = restored_sessions(persisted.as_ref(), &initial_snapshot);
    let approvals = ApprovalStore::with_timeout(Duration::from_secs(config.approval.timeout_secs));
    let prompts = PromptStore::new();
    let (device_event_tx, mut device_event_rx) = mpsc::unbounded_channel();
    let device_manager = DeviceManager::start(
        config.device.clone(),
        factory,
        Some(initial_snapshot.clone()),
        device_event_tx,
    );

    let app_state = AppState {
        emotion_analyzer: EmotionAnalyzer::from_config(&config.emotion),
        config,
        state: Arc::new(Mutex::new(AgentState::new(initial_snapshot, initial_sessions))),
        pending_generation: Arc::new(AtomicU64::new(0)),
        emotion_generation: Arc::new(AtomicU64::new(0)),
        device_manager,
        approvals,
        prompts,
    };

    let approvals_for_device = app_state.approvals.clone();
    let prompts_for_device = app_state.prompts.clone();
    let approval_sync_state = Arc::new(app_state.clone());
    tokio::spawn(async move {
        while let Some(event) = device_event_rx.recv().await {
            match event {
                DeviceEvent::ApprovalResolved {
                    id,
                    decision,
                } => {
                    if let Some(approval_id) = approvals_for_device.resolve(&id, decision).await {
                        info!(approval_id, ?decision, "device resolved approval");
                        approval_sync_state.sync_device_approvals().await;
                    } else {
                        warn!(
                            approval_id = id,
                            ?decision,
                            "dropping approval result for unknown request"
                        );
                    }
                }
                DeviceEvent::PromptResolved {
                    id,
                    selection_index,
                } => {
                    if let Some(prompt_id) = prompts_for_device.resolve(&id, selection_index).await {
                        info!(prompt_id, selection_index, "device resolved prompt");
                        approval_sync_state.sync_device_approvals().await;
                    } else {
                        warn!(
                            prompt_id = id,
                            selection_index,
                            "dropping prompt result for unknown request"
                        );
                    }
                }
            }
        }
    });

    app_state
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
        .route("/v1/claude/prompts", post(post_prompt))
        .route("/v1/claude/prompts/{id}", get(get_prompt))
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
        sync::{
            Arc, Mutex,
            atomic::{AtomicBool, Ordering as AtomicOrdering},
        },
        time::SystemTime,
    };

    use anyhow::Result;
    use reqwest::Client;

    use super::*;
    use crate::{
        device::DeviceSession,
        model::{
            Attention, PersistedSessionState, WaitingPrompt, WaitingPromptKind,
            WaitingPromptOption, WireFrame,
        },
    };

    #[test]
    fn resolved_serial_baud_prefers_env_override() {
        assert_eq!(resolved_serial_baud(Some(230_400), Some(460_800)), 230_400);
    }

    #[test]
    fn resolved_serial_baud_uses_config_when_env_missing() {
        assert_eq!(resolved_serial_baud(None, Some(230_400)), 230_400);
    }

    #[test]
    fn resolved_serial_baud_falls_back_to_default() {
        assert_eq!(resolved_serial_baud(None, None), compat::DEFAULT_SERIAL_BAUD);
    }

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

    #[derive(Clone)]
    struct PanicAfterHelloFactory {
        crashed: Arc<AtomicBool>,
    }

    impl SessionFactory for PanicAfterHelloFactory {
        fn connect(&self, _port: &str, _baud: u32) -> Result<Box<dyn DeviceSession>> {
            Ok(Box::new(PanicAfterHelloSession {
                sent_hello: false,
                crashed: self.crashed.clone(),
            }))
        }
    }

    struct PanicAfterHelloSession {
        sent_hello: bool,
        crashed: Arc<AtomicBool>,
    }

    impl DeviceSession for PanicAfterHelloSession {
        fn read_frame(&mut self, _timeout: Duration) -> Result<Option<WireFrame>> {
            if !self.sent_hello {
                self.sent_hello = true;
                return Ok(Some(WireFrame::Hello {
                    protocol_version: 1,
                    device_id: "esp32-dashboard".into(),
                    product: "waveshare".into(),
                    capabilities: vec!["claude.update".into()],
                }));
            }

            self.crashed.store(true, AtomicOrdering::SeqCst);
            panic!("simulated device worker crash");
        }

        fn write_frame(&mut self, _frame: &WireFrame) -> Result<()> {
            Ok(())
        }
    }

    fn approval_request_ids(writes: &[WireFrame]) -> Vec<String> {
        writes
            .iter()
            .filter_map(|frame| match frame {
                WireFrame::Event {
                    method,
                    payload,
                } if method == "claude.approval.request" => {
                    payload["id"].as_str().map(ToOwned::to_owned)
                }
                _ => None,
            })
            .collect()
    }

    fn has_approval_dismiss(writes: &[WireFrame], id: &str) -> bool {
        writes.iter().any(|frame| {
            matches!(
                frame,
                WireFrame::Event { method, payload }
                    if method == "claude.approval.dismiss"
                        && payload["id"] == id
            )
        })
    }

    fn prompt_request_ids(writes: &[WireFrame]) -> Vec<String> {
        writes
            .iter()
            .filter_map(|frame| match frame {
                WireFrame::Event {
                    method,
                    payload,
                } if method == "claude.prompt.request" => {
                    payload["id"].as_str().map(ToOwned::to_owned)
                }
                _ => None,
            })
            .collect()
    }

    fn has_prompt_dismiss(writes: &[WireFrame], id: &str) -> bool {
        writes.iter().any(|frame| {
            matches!(
                frame,
                WireFrame::Event { method, payload }
                    if method == "claude.prompt.dismiss"
                        && payload["id"] == id
            )
        })
    }

    fn waiting_prompt_event(
        session_id: &str,
        hook_event_name: &str,
        tool_name: Option<&str>,
        tool_use_id: Option<&str>,
        prompt: Option<WaitingPrompt>,
    ) -> LocalHookEvent {
        LocalHookEvent {
            session_id: session_id.into(),
            cwd: "/tmp/project".into(),
            hook_event_name: hook_event_name.into(),
            message: None,
            prompt_preview: None,
            prompt_raw: None,
            tool_name: tool_name.map(str::to_string),
            tool_use_id: tool_use_id.map(str::to_string),
            permission_mode: "default".into(),
            waiting_prompt: prompt,
            recv_ts: 123,
            claude_pid: None,
        }
    }

    fn sample_waiting_prompt(kind: WaitingPromptKind) -> WaitingPrompt {
        WaitingPrompt {
            kind,
            title: "Awaiting input".into(),
            question: "Choose an action".into(),
            options: vec![
                WaitingPromptOption {
                    label: "Execute".into(),
                    description: Some("Run the approved plan".into()),
                },
                WaitingPromptOption {
                    label: "More prompt".into(),
                    description: Some("Edit the request in terminal".into()),
                },
            ],
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
                rpc_timeout_approval: Duration::from_secs(310),
            },
            emotion: EmotionConfig::default(),
            approval: ApprovalConfig::default(),
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
                rpc_timeout_approval: Duration::from_secs(310),
            },
            emotion: EmotionConfig::default(),
            approval: ApprovalConfig::default(),
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
                prompt_raw: None,
                tool_name: None,
                tool_use_id: None,
                permission_mode: "default".into(),
                waiting_prompt: None,
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
                rpc_timeout_approval: Duration::from_secs(310),
            },
            emotion: EmotionConfig::default(),
            approval: ApprovalConfig::default(),
        };

        let app_state = build_app_state(
            config,
            Arc::new(FakeFactory {
                writes: writes.clone(),
            }),
        );
        let approval_state = Arc::new(app_state.clone());
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
            .post(format!("http://{addr}/v1/claude/approvals"))
            .json(&serde_json::json!({
                "id": "approval-2",
                "tool_name": "Edit",
                "tool_input_summary": "edit src/main.rs",
                "permission_mode": "default",
                "session_id": "sess-2",
                "tool_use_id": "tool-2",
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
                prompt_raw: None,
                tool_name: Some("Bash".into()),
                tool_use_id: Some("tool-1".into()),
                permission_mode: "default".into(),
                waiting_prompt: None,
                recv_ts: 124,
                claude_pid: None,
            })
            .send()
            .await?
            .error_for_status()?;

        approval_state.sync_device_approvals().await;
        tokio::time::sleep(Duration::from_millis(100)).await;

        let writes = writes.lock().expect("writes mutex poisoned");
        let request_ids = approval_request_ids(&writes);
        assert_eq!(request_ids, vec!["approval-1".to_string(), "approval-2".to_string()]);
        assert!(has_approval_dismiss(&writes, "approval-1"));

        server.abort();
        Ok(())
    }

    #[tokio::test]
    async fn waiting_prompt_is_forwarded_and_dismissed_on_follow_up_event() -> Result<()> {
        let unique = SystemTime::now().duration_since(SystemTime::UNIX_EPOCH)?.as_nanos();
        let state_dir = std::env::temp_dir().join(format!("esp32dash-prompt-test-{unique}"));
        fs::create_dir_all(&state_dir)?;
        let writes = Arc::new(Mutex::new(Vec::new()));

        let config = Config {
            admin_addr: "127.0.0.1:0".parse()?,
            admin_addr_raw: "127.0.0.1:0".into(),
            state_path: state_dir.join("state.json"),
            device: DeviceConfig {
                preferred_port: Some("/dev/fake".into()),
                baud: 115_200,
                rpc_timeout_approval: Duration::from_secs(310),
            },
            emotion: EmotionConfig::default(),
            approval: ApprovalConfig::default(),
        };

        let app_state = Arc::new(build_app_state(
            config,
            Arc::new(FakeFactory {
                writes: writes.clone(),
            }),
        ));

        for _ in 0..20 {
            if app_state.serial_status().connected {
                break;
            }
            tokio::time::sleep(Duration::from_millis(10)).await;
        }
        assert!(app_state.serial_status().connected);

        app_state
            .ingest(waiting_prompt_event(
                "sess-1",
                "PreToolUse",
                Some("AskUserQuestion"),
                Some("tool-1"),
                Some(sample_waiting_prompt(WaitingPromptKind::AskUserQuestion)),
            ))
            .await;
        tokio::time::sleep(Duration::from_millis(350)).await;

        app_state
            .ingest(waiting_prompt_event(
                "sess-1",
                "PostToolUse",
                Some("AskUserQuestion"),
                Some("tool-1"),
                None,
            ))
            .await;
        tokio::time::sleep(Duration::from_millis(50)).await;

        let writes = writes.lock().expect("writes mutex poisoned");
        // Prompt no longer sent as device_link event; state synced via snapshot
        let prompt_ids = prompt_request_ids(&writes);
        assert_eq!(prompt_ids.len(), 0);

        Ok(())
    }

    #[tokio::test]
    async fn approval_preempts_visible_prompt_and_prompt_returns_after_resolution() -> Result<()> {
        let unique = SystemTime::now().duration_since(SystemTime::UNIX_EPOCH)?.as_nanos();
        let state_dir =
            std::env::temp_dir().join(format!("esp32dash-prompt-priority-test-{unique}"));
        fs::create_dir_all(&state_dir)?;
        let writes = Arc::new(Mutex::new(Vec::new()));

        let config = Config {
            admin_addr: "127.0.0.1:0".parse()?,
            admin_addr_raw: "127.0.0.1:0".into(),
            state_path: state_dir.join("state.json"),
            device: DeviceConfig {
                preferred_port: Some("/dev/fake".into()),
                baud: 115_200,
                rpc_timeout_approval: Duration::from_secs(310),
            },
            emotion: EmotionConfig::default(),
            approval: ApprovalConfig::default(),
        };

        let app_state = Arc::new(build_app_state(
            config,
            Arc::new(FakeFactory {
                writes: writes.clone(),
            }),
        ));

        for _ in 0..20 {
            if app_state.serial_status().connected {
                break;
            }
            tokio::time::sleep(Duration::from_millis(10)).await;
        }
        assert!(app_state.serial_status().connected);

        app_state
            .ingest(waiting_prompt_event(
                "sess-prompt",
                "Elicitation",
                None,
                None,
                Some(sample_waiting_prompt(WaitingPromptKind::Elicitation)),
            ))
            .await;
        tokio::time::sleep(Duration::from_millis(50)).await;

        app_state
            .approvals
            .submit(
                "approve-1".into(),
                "approval-1".into(),
                ApprovalRequest {
                    tool_name: "Bash".into(),
                    tool_input_summary: "rm -rf /tmp/test".into(),
                    permission_mode: "default".into(),
                    session_id: "sess-approval".into(),
                    tool_use_id: Some("tool-approve".into()),
                    permission_suggestions: Vec::new(),
                },
            )
            .await;
        app_state.sync_device_approvals().await;
        tokio::time::sleep(Duration::from_millis(50)).await;

        assert_eq!(
            app_state
                .approvals
                .resolve("approval-1", crate::approvals::ApprovalDecision::Allow)
                .await,
            Some("approve-1".into())
        );
        app_state.sync_device_approvals().await;
        tokio::time::sleep(Duration::from_millis(50)).await;

        let writes = writes.lock().expect("writes mutex poisoned");
        // Prompt no longer sent as device_link event; only approval is forwarded
        let prompt_ids = prompt_request_ids(&writes);
        assert_eq!(prompt_ids.len(), 0);
        let approval_ids = approval_request_ids(&writes);
        assert_eq!(approval_ids, vec!["approval-1".to_string()]);

        Ok(())
    }

    #[tokio::test]
    async fn host_serializes_multiple_approvals_until_current_resolves() -> Result<()> {
        let unique = SystemTime::now().duration_since(SystemTime::UNIX_EPOCH)?.as_nanos();
        let state_dir = std::env::temp_dir().join(format!("esp32dash-queue-test-{unique}"));
        fs::create_dir_all(&state_dir)?;
        let writes = Arc::new(Mutex::new(Vec::new()));

        let config = Config {
            admin_addr: "127.0.0.1:0".parse()?,
            admin_addr_raw: "127.0.0.1:0".into(),
            state_path: state_dir.join("state.json"),
            device: DeviceConfig {
                preferred_port: Some("/dev/fake".into()),
                baud: 115_200,
                rpc_timeout_approval: Duration::from_secs(310),
            },
            emotion: EmotionConfig::default(),
            approval: ApprovalConfig::default(),
        };

        let app_state = build_app_state(
            config,
            Arc::new(FakeFactory {
                writes: writes.clone(),
            }),
        );
        let approval_state = Arc::new(app_state.clone());
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
            .post(format!("http://{addr}/v1/claude/approvals"))
            .json(&serde_json::json!({
                "id": "approval-2",
                "tool_name": "Edit",
                "tool_input_summary": "edit src/main.rs",
                "permission_mode": "default",
                "session_id": "sess-2",
                "tool_use_id": "tool-2",
            }))
            .send()
            .await?
            .error_for_status()?;

        tokio::time::sleep(Duration::from_millis(100)).await;

        {
            let writes = writes.lock().expect("writes mutex poisoned");
            let request_ids = approval_request_ids(&writes);
            assert_eq!(request_ids, vec!["approval-1".to_string()]);
        }

        assert_eq!(
            approval_state
                .approvals
                .resolve("approval-1", crate::approvals::ApprovalDecision::Allow)
                .await,
            Some("approval-1".into())
        );
        approval_state.sync_device_approvals().await;
        tokio::time::sleep(Duration::from_millis(50)).await;

        let writes = writes.lock().expect("writes mutex poisoned");
        let request_ids = approval_request_ids(&writes);
        assert_eq!(request_ids, vec!["approval-1".to_string(), "approval-2".to_string()]);

        server.abort();
        Ok(())
    }

    #[tokio::test]
    async fn failed_device_forward_does_not_immediately_requeue_claim() -> Result<()> {
        let unique = SystemTime::now().duration_since(SystemTime::UNIX_EPOCH)?.as_nanos();
        let state_dir = std::env::temp_dir().join(format!("esp32dash-approval-fail-{unique}"));
        fs::create_dir_all(&state_dir)?;
        let crashed = Arc::new(AtomicBool::new(false));

        let config = Config {
            admin_addr: "127.0.0.1:0".parse()?,
            admin_addr_raw: "127.0.0.1:0".into(),
            state_path: state_dir.join("state.json"),
            device: DeviceConfig {
                preferred_port: Some("/dev/fake".into()),
                baud: 115_200,
                rpc_timeout_approval: Duration::from_secs(310),
            },
            emotion: EmotionConfig::default(),
            approval: ApprovalConfig::default(),
        };

        let app_state = build_app_state(
            config,
            Arc::new(PanicAfterHelloFactory {
                crashed: crashed.clone(),
            }),
        );
        let approval_state = Arc::new(app_state.clone());

        for _ in 0..20 {
            if approval_state.serial_status().connected {
                break;
            }
            tokio::time::sleep(Duration::from_millis(10)).await;
        }
        assert!(approval_state.serial_status().connected);

        for _ in 0..20 {
            if crashed.load(AtomicOrdering::SeqCst) {
                break;
            }
            tokio::time::sleep(Duration::from_millis(20)).await;
        }
        assert!(crashed.load(AtomicOrdering::SeqCst));
        assert!(approval_state.serial_status().connected);

        approval_state
            .approvals
            .submit(
                "approve-1".into(),
                "approval-1".into(),
                ApprovalRequest {
                    tool_name: "Bash".into(),
                    tool_input_summary: "rm -rf /tmp/test".into(),
                    permission_mode: "default".into(),
                    session_id: "sess-1".into(),
                    tool_use_id: Some("tool-1".into()),
                    permission_suggestions: Vec::new(),
                },
            )
            .await;

        approval_state.sync_device_approvals().await;
        assert!(approval_state.approvals.claim_next_for_device().await.is_none());

        approval_state.approvals.note_device_disconnected().await;
        let retried = approval_state.approvals.claim_next_for_device().await.unwrap();
        assert_eq!(retried.id, "approve-1");
        assert_eq!(retried.transport_id, "approval-1");

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

        let mut guard = AgentState::new(Snapshot::empty(1), sessions);
        assert!(reconcile_session_map(&mut guard, PIDLESS_ACTIVE_SESSION_TIMEOUT_SECS + 2));
        let session = guard.sessions.get("sess-1").expect("session should remain tracked");
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

    #[tokio::test]
    async fn stale_emotion_result_does_not_override_newer_generation() -> Result<()> {
        let unique = SystemTime::now().duration_since(SystemTime::UNIX_EPOCH)?.as_nanos();
        let state_dir = std::env::temp_dir().join(format!("esp32dash-emotion-stale-{unique}"));
        fs::create_dir_all(&state_dir)?;

        let app_state = Arc::new(build_app_state(
            Config {
                admin_addr: "127.0.0.1:0".parse()?,
                admin_addr_raw: "127.0.0.1:0".into(),
                state_path: state_dir.join("state.json"),
                device: DeviceConfig {
                    preferred_port: Some("/dev/fake".into()),
                    baud: 115_200,
                    rpc_timeout_approval: Duration::from_secs(310),
                },
                emotion: EmotionConfig::default(),
                approval: ApprovalConfig::default(),
            },
            Arc::new(FakeFactory {
                writes: Arc::new(Mutex::new(Vec::new())),
            }),
        ));

        {
            let mut guard = app_state.state.lock().await;
            guard.sessions.insert(
                "sess-1".into(),
                PersistedSessionState {
                    key: "sess-1".into(),
                    snapshot: Snapshot::empty(1),
                    cwd: "/tmp/project".into(),
                    last_activity_ts: 1,
                    claude_pid: None,
                },
            );
            guard.emotions.insert(
                "sess-1".into(),
                SessionEmotionEntry {
                    generation: 2,
                    state: EmotionState::default(),
                },
            );
        }

        app_state
            .apply_emotion_analysis(
                "sess-1".into(),
                1,
                10,
                EmotionAnalysis {
                    emotion: Emotion::Happy,
                    confidence: 0.9,
                },
            )
            .await;

        let guard = app_state.state.lock().await;
        let session = guard.sessions.get("sess-1").expect("session should remain present");
        assert_eq!(session.snapshot.emotion, "neutral");
        Ok(())
    }

    #[test]
    fn reset_persisted_emotions_forces_neutral_on_restore() {
        let mut persisted = PersistedState {
            seq: 3,
            snapshot: Snapshot {
                emotion: "happy".into(),
                ..Snapshot::empty(42)
            },
            sessions: vec![PersistedSessionState {
                key: "sess-1".into(),
                snapshot: Snapshot {
                    emotion: "sad".into(),
                    ..Snapshot::empty(42)
                },
                cwd: "/tmp/project".into(),
                last_activity_ts: 42,
                claude_pid: None,
            }],
        };

        reset_persisted_emotions(&mut persisted);
        assert_eq!(persisted.snapshot.emotion, "neutral");
        assert_eq!(persisted.sessions[0].snapshot.emotion, "neutral");
    }

    #[test]
    fn process_exists_reports_current_process_alive() {
        assert!(process_exists(process::id()));
    }
}
