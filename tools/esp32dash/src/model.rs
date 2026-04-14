use std::collections::BTreeMap;

use serde::{Deserialize, Serialize};
use serde_json::Value;

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
    #[serde(default)]
    pub tool_input: Option<Value>,
    #[serde(flatten, default)]
    pub extra: BTreeMap<String, Value>,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "snake_case")]
pub enum WaitingPromptKind {
    AskUserQuestion,
    Elicitation,
}

impl WaitingPromptKind {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::AskUserQuestion => "question_prompt",
            Self::Elicitation => "elicitation_prompt",
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct WaitingPromptOption {
    pub label: String,
    #[serde(default)]
    pub description: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct WaitingPrompt {
    pub kind: WaitingPromptKind,
    pub title: String,
    pub question: String,
    #[serde(default)]
    pub options: Vec<WaitingPromptOption>,
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
    #[serde(default)]
    pub waiting_prompt: Option<WaitingPrompt>,
    pub recv_ts: u64,
    #[serde(default)]
    pub claude_pid: Option<u32>,
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

    pub fn from_str(value: &str) -> Self {
        match value {
            "waiting_for_input" => Self::WaitingForInput,
            "processing" => Self::Processing,
            "running_tool" => Self::RunningTool,
            "compacting" => Self::Compacting,
            "ended" => Self::Ended,
            _ => Self::Unknown,
        }
    }

    pub fn is_active(self) -> bool {
        matches!(
            self,
            Self::WaitingForInput | Self::Processing | Self::RunningTool | Self::Compacting
        )
    }

    pub fn display_priority(self) -> u8 {
        match self {
            Self::WaitingForInput => 5,
            Self::RunningTool => 4,
            Self::Processing => 3,
            Self::Compacting => 2,
            Self::Unknown => 1,
            Self::Ended => 0,
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

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
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
            detail: "".to_string(),
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
    #[serde(default)]
    pub sessions: Vec<PersistedSessionState>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PersistedSessionState {
    pub key: String,
    pub snapshot: Snapshot,
    #[serde(default)]
    pub cwd: String,
    #[serde(default)]
    pub last_activity_ts: u64,
    #[serde(default)]
    pub claude_pid: Option<u32>,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
pub struct DeviceHello {
    pub protocol_version: u32,
    pub device_id: String,
    pub product: String,
    #[serde(default)]
    pub capabilities: Vec<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
pub struct SerialConnectionStatus {
    pub configured_port: Option<String>,
    pub active_port: Option<String>,
    pub connected: bool,
    pub protocol_version: Option<u32>,
    pub device_id: Option<String>,
    pub product: Option<String>,
    #[serde(default)]
    pub capabilities: Vec<String>,
    pub last_error: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct AgentStatusResponse {
    pub admin_addr: String,
    pub current_seq: u64,
    pub serial: SerialConnectionStatus,
    pub snapshot: Snapshot,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DeviceListEntry {
    pub port: String,
    pub hello: DeviceHello,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ScreenshotStartResponse {
    pub capture_id: String,
    pub app: String,
    pub source: String,
    pub format: String,
    pub width: u16,
    pub height: u16,
    pub stride_bytes: u16,
    pub data_size: u32,
    pub chunk_bytes: u32,
    pub chunk_count: u32,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ScreenshotChunkEvent {
    pub capture_id: String,
    pub index: u32,
    pub data: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ScreenshotDoneEvent {
    pub capture_id: String,
    pub chunks_sent: u32,
    pub bytes_sent: u32,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ScreenshotErrorEvent {
    pub capture_id: String,
    pub message: String,
}

#[derive(Debug, Clone)]
pub struct DeviceScreenshot {
    pub meta: ScreenshotStartResponse,
    pub data: Vec<u8>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct RpcRequest {
    pub method: String,
    #[serde(default)]
    pub params: Value,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct AdminRpcResponse {
    pub ok: bool,
    #[serde(default)]
    pub result: Option<Value>,
    #[serde(default)]
    pub error: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct AdminErrorResponse {
    pub ok: bool,
    pub error: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct WireError {
    pub code: String,
    pub message: String,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum WireFrame {
    Hello {
        protocol_version: u32,
        device_id: String,
        product: String,
        #[serde(default)]
        capabilities: Vec<String>,
    },
    Request {
        id: String,
        method: String,
        #[serde(default)]
        params: Value,
    },
    Response {
        id: String,
        ok: bool,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        result: Option<Value>,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        error: Option<WireError>,
    },
    Event {
        method: String,
        payload: Value,
    },
}

impl WireFrame {
    pub fn event(method: impl Into<String>, payload: Value) -> Self {
        Self::Event {
            method: method.into(),
            payload,
        }
    }

    pub fn request(id: impl Into<String>, method: impl Into<String>, params: Value) -> Self {
        Self::Request {
            id: id.into(),
            method: method.into(),
            params,
        }
    }

    pub fn into_hello(self) -> Option<DeviceHello> {
        match self {
            Self::Hello {
                protocol_version,
                device_id,
                product,
                capabilities,
            } => Some(DeviceHello {
                protocol_version,
                device_id,
                product,
                capabilities,
            }),
            _ => None,
        }
    }
}
