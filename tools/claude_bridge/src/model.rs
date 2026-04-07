use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RawHookInput {
    #[serde(default)]
    pub session_id: String,
    #[serde(default)]
    pub cwd: String,
    #[serde(default)]
    pub hook_event_name: String,
    #[serde(default)]
    pub message: Option<String>,
    #[serde(default)]
    pub prompt: Option<String>,
    #[serde(default)]
    pub tool_name: Option<String>,
    #[serde(default)]
    pub tool_use_id: Option<String>,
    #[serde(default)]
    pub permission_mode: Option<String>,
    #[serde(default)]
    pub reason: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct LocalHookEvent {
    pub session_id: String,
    pub cwd: String,
    pub hook_event_name: String,
    pub message: Option<String>,
    pub prompt_preview: Option<String>,
    pub tool_name: Option<String>,
    pub tool_use_id: Option<String>,
    pub permission_mode: String,
    pub recv_ts: u64,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum RunStatus {
    WaitingForInput,
    Processing,
    RunningTool,
    Compacting,
    Ended,
    Unknown,
}

impl RunStatus {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::WaitingForInput => "waiting_for_input",
            Self::Processing => "processing",
            Self::RunningTool => "running_tool",
            Self::Compacting => "compacting",
            Self::Ended => "ended",
            Self::Unknown => "unknown",
        }
    }
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum Attention {
    Low,
    Medium,
    High,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Snapshot {
    pub seq: u64,
    pub source: String,
    pub session_id: String,
    pub event: String,
    pub status: String,
    pub title: String,
    pub workspace: String,
    pub detail: String,
    pub permission_mode: String,
    pub ts: u64,
    pub unread: bool,
    pub attention: Attention,
}

impl Snapshot {
    pub fn empty(ts: u64) -> Self {
        Self {
            seq: 0,
            source: "claude_code".to_string(),
            session_id: String::new(),
            event: "none".to_string(),
            status: RunStatus::Unknown.as_str().to_string(),
            title: "No data yet".to_string(),
            workspace: String::new(),
            detail: "Bridge has not received any Claude event yet".to_string(),
            permission_mode: "default".to_string(),
            ts,
            unread: false,
            attention: Attention::Low,
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PersistedState {
    pub seq: u64,
    pub snapshot: Snapshot,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct HelloMessage {
    pub r#type: String,
    pub token: String,
    pub device_id: String,
    pub last_seq: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct BridgeEnvelope {
    pub r#type: String,
    pub payload: Snapshot,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ErrorEnvelope {
    pub r#type: String,
    pub code: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct StatusResponse {
    pub admin_addr: String,
    pub ws_addr: String,
    pub current_seq: u64,
    pub connected_clients: usize,
    pub snapshot: Snapshot,
}
