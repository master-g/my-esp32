mod agent;
mod approvals;
mod compat;
mod config;
mod config_ui;
mod device;
mod emotion;
mod emotion_state;
mod hooks;
mod launchd;
mod model;
mod normalizer;
mod prompts;
mod text_sanitize;

use std::{
    env,
    fs::{self, File},
    io::{self, BufWriter, Read},
    path::PathBuf,
    sync::{
        Arc,
        atomic::{AtomicU64, Ordering},
    },
};

use anyhow::{Context, Result, anyhow};
use clap::{Args, Parser, Subcommand};
use reqwest::Client;
use serde::Serialize;
use serde_json::{Value, json};
use tracing::{debug, warn};
use tracing_subscriber::{EnvFilter, fmt};

use crate::{
    agent::Config,
    config::AppConfig,
    device::{
        UnixSerialFactory, capture_screenshot_direct, discover_devices, open_direct_session,
        request_direct, request_direct_with_timeout, send_event_direct, send_protocol_event_direct,
    },
    hooks::InstallHooksResult,
    model::{
        AdminErrorResponse, AdminRpcResponse, Attention, DeviceScreenshot, LocalHookEvent,
        RawHookInput, RpcRequest, RunStatus, Snapshot, WaitingPrompt, WaitingPromptKind,
        WaitingPromptOption, WireFrame,
    },
    text_sanitize::{
        build_approval_id, sanitize_approval_summary, sanitize_message, sanitize_permission_mode,
        sanitize_prompt_preview, sanitize_snapshot_detail, sanitize_snapshot_title,
        sanitize_tool_name,
    },
};

static NEXT_APPROVAL_ID_SEQ: AtomicU64 = AtomicU64::new(1);
static NEXT_PROMPT_ID_SEQ: AtomicU64 = AtomicU64::new(1);
const CHIBI_SCREENSAVER_RPC_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(5);

#[derive(Debug, Parser)]
#[command(
    name = "esp32dash",
    about = "Manage the ESP32 dashboard host agent, Claude hook ingestion, and serial device control",
    subcommand_required = true,
    arg_required_else_help = true
)]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Debug, Subcommand)]
enum Command {
    #[command(
        about = "Run or inspect the local esp32dash host agent",
        subcommand_required = true,
        arg_required_else_help = true
    )]
    Agent {
        #[command(subcommand)]
        command: AgentCommand,
    },
    #[command(
        about = "Forward Claude Code hook events into the local agent",
        subcommand_required = true,
        arg_required_else_help = true
    )]
    Claude {
        #[command(subcommand)]
        command: ClaudeCommand,
    },
    #[command(
        about = "Inspect or control the attached ESP32 dashboard over serial",
        subcommand_required = true,
        arg_required_else_help = true
    )]
    Device {
        #[command(subcommand)]
        command: DeviceCommand,
    },
    #[command(about = "Edit Wi-Fi, timezone, and weather settings interactively")]
    Config(ConfigArgs),
    #[command(about = "Install esp32dash as a launchd service on macOS")]
    InstallLaunchd,
    #[command(about = "Remove the esp32dash launchd service on macOS")]
    UninstallLaunchd,
    #[command(
        about = "Install the Claude hook wrapper and register hooks in ~/.claude/settings.json"
    )]
    InstallHooks(InstallHooksArgs),
    #[command(
        about = "Test chibi sprite states and text bubbles on the ESP32 display",
        subcommand_required = true,
        arg_required_else_help = true
    )]
    Chibi {
        #[command(subcommand)]
        command: ChibiCommand,
    },
}

#[derive(Debug, Subcommand)]
enum AgentCommand {
    #[command(about = "Run the local esp32dash agent")]
    Run,
    #[command(
        about = "Show the agent state, current serial connection, and latest Claude snapshot"
    )]
    Status,
}

#[derive(Debug, Subcommand)]
enum ClaudeCommand {
    #[command(about = "Read a Claude hook event JSON object from stdin and ingest it")]
    Ingest {
        #[arg(long, help = "Read the hook event payload from stdin")]
        event_from_stdin: bool,
    },
    #[command(
        about = "Handle a user-input hook event (PermissionRequest, Elicitation, AskUserQuestion): submit to agent, wait for device response, output decision JSON",
        alias = "approve"
    )]
    Respond {
        #[arg(long, help = "Read the hook event payload from stdin")]
        event_from_stdin: bool,
    },
}

#[derive(Debug, Subcommand)]
enum DeviceCommand {
    #[command(about = "List serial ports that speak the esp32dash protocol")]
    List,
    #[command(about = "Fetch basic metadata from the device")]
    Info {
        #[arg(
            long,
            help = "Use a specific serial port instead of autodiscovery or the running agent"
        )]
        port: Option<String>,
    },
    #[command(about = "Request a soft reboot from the device")]
    Reboot {
        #[arg(
            long,
            help = "Use a specific serial port instead of autodiscovery or the running agent"
        )]
        port: Option<String>,
    },
    #[command(about = "Capture the current device screen to a PNG file over direct serial")]
    Screenshot {
        #[arg(short = 'o', long, help = "Output PNG path")]
        out: PathBuf,
        #[arg(long, help = "Use a specific serial port instead of autodiscovery")]
        port: Option<String>,
    },
}

#[derive(Debug, Args)]
struct ConfigArgs {
    #[arg(long, help = "Use a specific serial port instead of autodiscovery or the running agent")]
    port: Option<String>,
}

#[derive(Debug, Args)]
struct InstallHooksArgs {
    #[arg(short = 'f', long, help = "Overwrite and update without interactive confirmation")]
    force: bool,
}

#[derive(Debug, Serialize)]
struct ScreenshotCommandResult {
    ok: bool,
    path: String,
    app: String,
    source: String,
    format: String,
    width: u16,
    height: u16,
    bytes: usize,
}

#[derive(Debug, Clone, clap::ValueEnum)]
enum ChibiState {
    Idle,
    Working,
    Waiting,
    Compacting,
    Sleeping,
}

#[derive(Debug, Clone, clap::ValueEnum)]
enum ChibiEmotion {
    Neutral,
    Happy,
    Sad,
    Sob,
}

#[derive(Debug, Clone, Copy, clap::ValueEnum)]
enum ChibiUnread {
    Auto,
    On,
    Off,
}

#[derive(Debug, Clone, Copy, clap::ValueEnum)]
enum ChibiAttention {
    Auto,
    Low,
    Medium,
    High,
}

#[derive(Debug, Clone, clap::ValueEnum)]
enum ScreensaverAction {
    Enter,
    Exit,
}

impl ChibiState {
    fn to_run_status(&self) -> RunStatus {
        match self {
            Self::Idle => RunStatus::Unknown,
            Self::Working => RunStatus::Processing,
            Self::Waiting => RunStatus::WaitingForInput,
            Self::Compacting => RunStatus::Compacting,
            Self::Sleeping => RunStatus::Ended,
        }
    }
}

impl ChibiEmotion {
    fn as_str(&self) -> &'static str {
        match self {
            Self::Neutral => "neutral",
            Self::Happy => "happy",
            Self::Sad => "sad",
            Self::Sob => "sob",
        }
    }
}

impl ChibiUnread {
    fn resolve(self, state: &ChibiState) -> bool {
        match self {
            Self::Auto => matches!(state, ChibiState::Waiting),
            Self::On => true,
            Self::Off => false,
        }
    }
}

impl ChibiAttention {
    fn resolve(self, unread: bool) -> Attention {
        match self {
            Self::Auto => {
                if unread {
                    Attention::Medium
                } else {
                    Attention::Low
                }
            }
            Self::Low => Attention::Low,
            Self::Medium => Attention::Medium,
            Self::High => Attention::High,
        }
    }
}

#[derive(Debug, Subcommand)]
enum ChibiCommand {
    #[command(
        about = "Send a custom Home snapshot to the device for sprite, bubble, and badge testing"
    )]
    Test {
        #[arg(long, value_enum, help = "Sprite state to display")]
        state: ChibiState,
        #[arg(long, value_enum, default_value = "neutral", help = "Emotion variant to display")]
        emotion: ChibiEmotion,
        #[arg(long, help = "Text to show in the speech bubble")]
        bubble: Option<String>,
        #[arg(long, value_enum, default_value = "auto", help = "Unread badge behavior")]
        unread: ChibiUnread,
        #[arg(long, value_enum, default_value = "auto", help = "Attention level to send")]
        attention: ChibiAttention,
        #[arg(long, default_value = "Chibi Test", help = "Snapshot title to send")]
        title: String,
        #[arg(long, default_value = "chibi_test", help = "Snapshot source label")]
        source: String,
        #[arg(long, default_value = "test-session", help = "Session id to send")]
        session_id: String,
        #[arg(long, default_value = "Stop", help = "Hook event label to send")]
        event: String,
        #[arg(long, help = "Serial port (default: autodiscover)")]
        port: Option<String>,
    },
    #[command(about = "Cycle through all sprite states with sample bubbles")]
    Demo {
        #[arg(long, help = "Serial port (default: autodiscover)")]
        port: Option<String>,
    },
    #[command(about = "Test the permission approval overlay on the device")]
    Approve {
        #[arg(long, default_value = "Bash", help = "Tool name to show")]
        tool: String,
        #[arg(long, default_value = "rm -rf /tmp/test", help = "Description to show")]
        desc: String,
        #[arg(long, help = "Serial port (default: autodiscover)")]
        port: Option<String>,
    },
    #[command(
        about = "Show the approval overlay, then dismiss it as if approval happened on the host"
    )]
    ApproveDismiss {
        #[arg(long, default_value = "Bash", help = "Tool name to show")]
        tool: String,
        #[arg(long, default_value = "rm -rf /tmp/test", help = "Description to show")]
        desc: String,
        #[arg(
            long,
            default_value_t = 1500,
            help = "Milliseconds to keep the overlay visible before dismissing it"
        )]
        delay_ms: u64,
        #[arg(long, help = "Serial port for direct fallback when the local agent is unavailable")]
        port: Option<String>,
    },
    #[command(about = "Force the Home screensaver on or off for quick testing")]
    Screensaver {
        #[arg(value_enum, help = "Whether to enter or exit the screensaver")]
        action: ScreensaverAction,
        #[arg(long, help = "Serial port (default: autodiscover or running agent)")]
        port: Option<String>,
    },
    #[command(
        about = "Send a test approval request to the device via serial",
        long_about = "Sends claude.approval.request directly to the device and blocks for a response. Use --clear to dismiss instead."
    )]
    Approval {
        #[arg(long, default_value = "Bash", help = "Tool name to show")]
        tool: String,
        #[arg(long, default_value = "rm -rf /tmp/test", help = "Description to show")]
        input: String,
        #[arg(long, help = "Dismiss the approval instead of sending a request")]
        clear: bool,
        #[arg(long, help = "Serial port (default: autodiscover)")]
        port: Option<String>,
    },
    #[command(
        about = "Send a test prompt request to the device via serial",
        long_about = "Sends claude.prompt.request directly to the device (non-blocking). Use --clear to dismiss instead."
    )]
    Prompt {
        #[arg(long, default_value = "Execute,Cancel", help = "Comma-separated option labels")]
        options: String,
        #[arg(long, default_value = "Test prompt question?", help = "Question text")]
        question: String,
        #[arg(long, default_value = "Test Prompt", help = "Title text")]
        title: String,
        #[arg(long, help = "Dismiss the prompt instead of sending a request")]
        clear: bool,
        #[arg(long, help = "Serial port (default: autodiscover)")]
        port: Option<String>,
    },
}

#[tokio::main]
async fn main() -> Result<()> {
    init_tracing();
    let cli = Cli::parse();

    match cli.command {
        Command::Agent {
            command: AgentCommand::Run,
        } => {
            let config = Config::from_env()?;
            agent::run(config).await
        }
        Command::Agent {
            command: AgentCommand::Status,
        } => agent_status().await,
        Command::Claude {
            command: ClaudeCommand::Ingest {
                event_from_stdin,
            },
        } => {
            if !event_from_stdin {
                return Err(anyhow!("ingest requires --event-from-stdin"));
            }
            ingest_from_stdin().await
        }
        Command::Claude {
            command: ClaudeCommand::Respond {
                event_from_stdin,
            },
        } => {
            if !event_from_stdin {
                return Err(anyhow!("respond requires --event-from-stdin"));
            }
            respond_from_stdin().await
        }
        Command::Device {
            command,
        } => run_device_command(command).await,
        Command::Config(args) => config_ui::run_config_editor(args.port).await,
        Command::InstallLaunchd => install_launchd(),
        Command::UninstallLaunchd => uninstall_launchd(),
        Command::InstallHooks(args) => install_hooks(args),
        Command::Chibi {
            command,
        } => run_chibi_command(command).await,
    }
}

async fn run_device_command(command: DeviceCommand) -> Result<()> {
    match command {
        DeviceCommand::List => {
            let devices = discover_devices(Arc::new(UnixSerialFactory), compat::serial_baud())?;
            print_json(&devices)
        }
        DeviceCommand::Info {
            port,
        } => {
            let result = run_request(
                port.as_deref(),
                RpcRequest {
                    method: "device.info".into(),
                    params: json!({}),
                },
            )
            .await?;
            print_json(&result)
        }
        DeviceCommand::Reboot {
            port,
        } => {
            let result = run_request(
                port.as_deref(),
                RpcRequest {
                    method: "device.reboot".into(),
                    params: json!({}),
                },
            )
            .await?;
            print_json(&result)
        }
        DeviceCommand::Screenshot {
            out,
            port,
        } => {
            let screenshot = capture_screenshot_direct(
                Arc::new(UnixSerialFactory),
                port.as_deref(),
                compat::serial_baud(),
            )?;
            write_screenshot_png(&out, &screenshot)?;
            let output_path = out.canonicalize().unwrap_or(out);

            print_json(&ScreenshotCommandResult {
                ok: true,
                path: output_path.to_string_lossy().into_owned(),
                app: screenshot.meta.app,
                source: screenshot.meta.source,
                format: screenshot.meta.format,
                width: screenshot.meta.width,
                height: screenshot.meta.height,
                bytes: screenshot.data.len(),
            })
        }
    }
}

pub(crate) async fn run_request(port: Option<&str>, request: RpcRequest) -> Result<Value> {
    if port.is_none() {
        match rpc_via_agent(request.clone()).await {
            Ok(result) => return Ok(result),
            Err(err) => {
                debug!("local agent unavailable, falling back to direct serial rpc: {err:#}")
            }
        }
    }

    request_direct(Arc::new(UnixSerialFactory), port, compat::serial_baud(), request)
}

async fn run_chibi_command(command: ChibiCommand) -> Result<()> {
    match command {
        ChibiCommand::Test {
            state,
            emotion,
            bubble,
            unread,
            attention,
            title,
            source,
            session_id,
            event,
            port,
        } => {
            let snapshot = build_test_snapshot(
                &state,
                &emotion,
                bubble.as_deref(),
                &ChibiSnapshotOptions {
                    unread,
                    attention,
                    title,
                    source,
                    session_id,
                    event,
                },
            );
            send_event_direct(
                Arc::new(UnixSerialFactory),
                port.as_deref(),
                compat::serial_baud(),
                &snapshot,
            )?;
            println!(
                "sent state={} emotion={} bubble={:?}",
                snapshot.status,
                snapshot.emotion,
                if snapshot.detail.is_empty() {
                    None
                } else {
                    Some(&snapshot.detail)
                }
            );
            Ok(())
        }
        ChibiCommand::Demo {
            port,
        } => {
            let factory = Arc::new(UnixSerialFactory);
            let baud = compat::serial_baud();
            let mut session = open_direct_session(factory, port.as_deref(), baud)?;

            let steps: &[(ChibiState, ChibiEmotion, &str)] = &[
                (ChibiState::Idle, ChibiEmotion::Happy, "Just chilling..."),
                (ChibiState::Working, ChibiEmotion::Happy, "Making progress"),
                (ChibiState::Working, ChibiEmotion::Sad, "That test failed"),
                (ChibiState::Compacting, ChibiEmotion::Neutral, "Compacting..."),
                (ChibiState::Waiting, ChibiEmotion::Sad, "Need your input!"),
                (ChibiState::Idle, ChibiEmotion::Sob, "This is rough..."),
                (ChibiState::Sleeping, ChibiEmotion::Happy, ""),
            ];

            for (i, (state, emotion, bubble)) in steps.iter().enumerate() {
                let bubble_text = if bubble.is_empty() {
                    None
                } else {
                    Some(*bubble)
                };
                let snapshot = build_test_snapshot(
                    state,
                    emotion,
                    bubble_text,
                    &ChibiSnapshotOptions::default(),
                );
                session.write_frame(&WireFrame::event(
                    "claude.update",
                    serde_json::to_value(&snapshot)?,
                ))?;
                println!(
                    "[{}/{}] state={} emotion={} bubble={:?}",
                    i + 1,
                    steps.len(),
                    snapshot.status,
                    snapshot.emotion,
                    bubble_text
                );
                if i + 1 < steps.len() {
                    std::thread::sleep(std::time::Duration::from_secs(3));
                }
            }
            println!("demo complete");
            Ok(())
        }
        ChibiCommand::Approve {
            tool,
            desc,
            port,
        } => {
            println!("sending approval request: tool={tool} desc=\"{desc}\"");
            println!("waiting for user to tap a button on the device...");

            let rpc = RpcRequest {
                method: "claude.approve".into(),
                params: json!({
                    "id": "chibi-test",
                    "tool_name": tool,
                    "description": desc,
                }),
            };

            let app_config = AppConfig::load().unwrap_or_default();
            let result = request_direct_with_timeout(
                Arc::new(UnixSerialFactory),
                port.as_deref(),
                compat::serial_baud(),
                rpc,
                std::time::Duration::from_secs(app_config.approval.timeout_secs),
            )?;

            let decision = result.get("decision").and_then(|v| v.as_str()).unwrap_or("unknown");
            println!("device responded: decision={decision}");
            Ok(())
        }
        ChibiCommand::ApproveDismiss {
            tool,
            desc,
            delay_ms,
            port,
        } => {
            let scenario = build_approve_dismiss_scenario(&tool, &desc);
            let client = Client::new();
            let base_url = admin_base_url();
            let delay = std::time::Duration::from_millis(delay_ms);

            if local_agent_available(&client, &base_url).await {
                println!(
                    "showing approval overlay via local agent: tool={} desc=\"{}\"",
                    scenario.display_tool_name, scenario.description
                );
                run_approve_dismiss_scenario(&client, &base_url, &scenario, delay).await?;
                println!("dismiss confirmed by local agent");
            } else {
                println!(
                    "local agent unavailable, falling back to direct serial smoke test: tool={} desc=\"{}\"",
                    scenario.display_tool_name, scenario.description
                );
                run_approve_dismiss_direct(port.as_deref(), &scenario, delay).await?;
                println!("direct dismiss sequence sent");
            }
            Ok(())
        }
        ChibiCommand::Screensaver {
            action,
            port,
        } => {
            let enabled = matches!(action, ScreensaverAction::Enter);
            request_direct_with_timeout(
                Arc::new(UnixSerialFactory),
                port.as_deref(),
                compat::serial_baud(),
                RpcRequest {
                    method: "home.screensaver".into(),
                    params: json!({
                        "enabled": enabled,
                    }),
                },
                CHIBI_SCREENSAVER_RPC_TIMEOUT,
            )?;
            println!(
                "screensaver {}",
                if enabled {
                    "entered"
                } else {
                    "exited"
                }
            );
            Ok(())
        }
        ChibiCommand::Approval {
            tool,
            input,
            clear,
            port,
        } => {
            if clear {
                send_protocol_event_direct(
                    Arc::new(UnixSerialFactory),
                    port.as_deref(),
                    compat::serial_baud(),
                    "claude.approval.dismiss",
                    json!({
                        "id": "chibi-approval-test",
                    }),
                )?;
                println!("sent approval dismiss");
                Ok(())
            } else {
                println!("sending approval request: tool={tool} input=\"{input}\"");
                println!("waiting for user to tap a button on the device...");
                let rpc = RpcRequest {
                    method: "claude.approve".into(),
                    params: json!({
                        "id": "chibi-approval-test",
                        "tool_name": tool,
                        "description": input,
                    }),
                };
                let app_config = AppConfig::load().unwrap_or_default();
                let result = request_direct_with_timeout(
                    Arc::new(UnixSerialFactory),
                    port.as_deref(),
                    compat::serial_baud(),
                    rpc,
                    std::time::Duration::from_secs(app_config.approval.timeout_secs),
                )?;
                let decision = result.get("decision").and_then(|v| v.as_str()).unwrap_or("unknown");
                println!("device responded: decision={decision}");
                Ok(())
            }
        }
        ChibiCommand::Prompt {
            options,
            question,
            title,
            clear,
            port,
        } => {
            if clear {
                send_protocol_event_direct(
                    Arc::new(UnixSerialFactory),
                    port.as_deref(),
                    compat::serial_baud(),
                    "claude.prompt.dismiss",
                    json!({
                        "id": "chibi-prompt-test",
                    }),
                )?;
                println!("sent prompt dismiss");
                Ok(())
            } else {
                let option_labels: Vec<String> = options.split(',').map(|s| s.trim().to_string()).collect();
                let option_count = option_labels.len();
                println!("sending prompt request: title=\"{title}\" question=\"{question}\"");
                send_protocol_event_direct(
                    Arc::new(UnixSerialFactory),
                    port.as_deref(),
                    compat::serial_baud(),
                    "claude.prompt.request",
                    json!({
                        "id": "chibi-prompt-test",
                        "title": title,
                        "question": question,
                        "options_text": options,
                        "option_count": option_count,
                        "option_labels": option_labels,
                    }),
                )?;
                println!("sent prompt request (non-blocking)");
                Ok(())
            }
        }
    }
}

#[derive(Debug, Clone)]
struct ChibiSnapshotOptions {
    unread: ChibiUnread,
    attention: ChibiAttention,
    title: String,
    source: String,
    session_id: String,
    event: String,
}

impl Default for ChibiSnapshotOptions {
    fn default() -> Self {
        Self {
            unread: ChibiUnread::Auto,
            attention: ChibiAttention::Auto,
            title: "Chibi Test".to_string(),
            source: "chibi_test".to_string(),
            session_id: "test-session".to_string(),
            event: "Stop".to_string(),
        }
    }
}

fn build_test_snapshot(
    state: &ChibiState,
    emotion: &ChibiEmotion,
    bubble: Option<&str>,
    options: &ChibiSnapshotOptions,
) -> Snapshot {
    let status = state.to_run_status();
    let unread = options.unread.resolve(state);
    let attention = options.attention.resolve(unread);
    Snapshot {
        seq: 1,
        source: options.source.clone(),
        session_id: options.session_id.clone(),
        event: options.event.clone(),
        status: status.as_str().to_string(),
        emotion: emotion.as_str().to_string(),
        title: options.title.clone(),
        workspace: String::new(),
        detail: bubble.unwrap_or_default().to_string(),
        permission_mode: "default".to_string(),
        ts: now_epoch(),
        unread,
        attention,
        has_pending_prompt: false,
    }
}

#[derive(Debug, Clone)]
struct ApproveDismissScenario {
    approval_id: String,
    display_tool_name: String,
    description: String,
    request_event: LocalHookEvent,
    clear_event: LocalHookEvent,
}

fn build_approve_dismiss_scenario(tool: &str, desc: &str) -> ApproveDismissScenario {
    let now_ms = now_epoch_millis();
    let display_tool_name = sanitize_tool_name(tool).unwrap_or_else(|| "unknown".into());
    let description = sanitize_approval_summary(desc);
    let approval_id = next_approval_id();
    let session_id = format!("approve-dismiss-{now_ms:x}");
    let tool_use_id = format!("tool-{approval_id}");
    let cwd =
        env::current_dir().map(|path| path.to_string_lossy().into_owned()).unwrap_or_default();
    let request_event = LocalHookEvent {
        session_id: session_id.clone(),
        cwd: cwd.clone(),
        hook_event_name: "PermissionRequest".into(),
        message: None,
        prompt_preview: None,
        prompt_raw: None,
        tool_name: Some(display_tool_name.clone()),
        tool_use_id: Some(tool_use_id.clone()),
        permission_mode: "default".into(),
        waiting_prompt: None,
        recv_ts: now_epoch(),
        claude_pid: None,
    };
    let clear_event = LocalHookEvent {
        session_id,
        cwd,
        hook_event_name: "PreToolUse".into(),
        message: None,
        prompt_preview: None,
        prompt_raw: None,
        tool_name: Some(display_tool_name.clone()),
        tool_use_id: Some(tool_use_id),
        permission_mode: "default".into(),
        waiting_prompt: None,
        recv_ts: now_epoch(),
        claude_pid: None,
    };

    ApproveDismissScenario {
        approval_id,
        display_tool_name,
        description,
        request_event,
        clear_event,
    }
}

async fn run_approve_dismiss_scenario(
    client: &Client,
    base_url: &str,
    scenario: &ApproveDismissScenario,
    delay: std::time::Duration,
) -> Result<()> {
    post_agent_event(client, base_url, &scenario.request_event).await?;
    submit_agent_approval(client, base_url, scenario).await?;
    tokio::time::sleep(delay).await;
    post_agent_event(client, base_url, &scenario.clear_event).await?;
    wait_for_approval_dismissal(client, base_url, &scenario.approval_id).await
}

async fn local_agent_available(client: &Client, base_url: &str) -> bool {
    match client
        .get(format!("{base_url}/v1/agent/status"))
        .timeout(std::time::Duration::from_millis(300))
        .send()
        .await
    {
        Ok(response) => response.status().is_success(),
        Err(_) => false,
    }
}

async fn post_agent_event(client: &Client, base_url: &str, event: &LocalHookEvent) -> Result<()> {
    client
        .post(format!("{base_url}/v1/claude/events"))
        .json(event)
        .timeout(std::time::Duration::from_secs(2))
        .send()
        .await
        .with_context(|| format!("failed to send {} event to local agent", event.hook_event_name))?
        .error_for_status()
        .with_context(|| format!("local agent rejected {} event", event.hook_event_name))?;
    Ok(())
}

async fn submit_agent_approval(
    client: &Client,
    base_url: &str,
    scenario: &ApproveDismissScenario,
) -> Result<()> {
    client
        .post(format!("{base_url}/v1/claude/approvals"))
        .json(&json!({
            "id": &scenario.approval_id,
            "tool_name": &scenario.display_tool_name,
            "tool_input_summary": &scenario.description,
            "permission_mode": "default",
            "session_id": &scenario.request_event.session_id,
            "tool_use_id": scenario.request_event.tool_use_id.as_deref(),
        }))
        .timeout(std::time::Duration::from_secs(2))
        .send()
        .await
        .context("failed to submit approval to local agent")?
        .error_for_status()
        .context("local agent rejected approval submission")?;
    Ok(())
}

async fn run_approve_dismiss_direct(
    port: Option<&str>,
    scenario: &ApproveDismissScenario,
    delay: std::time::Duration,
) -> Result<()> {
    let factory = Arc::new(UnixSerialFactory);
    let baud = compat::serial_baud();

    request_direct_with_timeout(
        factory.clone(),
        port,
        baud,
        RpcRequest {
            method: "home.screensaver".into(),
            params: json!({
                "enabled": false,
            }),
        },
        CHIBI_SCREENSAVER_RPC_TIMEOUT,
    )?;

    send_protocol_event_direct(
        factory.clone(),
        port,
        baud,
        "claude.approval.request",
        json!({
            "id": &scenario.approval_id,
            "tool_name": &scenario.display_tool_name,
            "description": &scenario.description,
        }),
    )?;

    tokio::time::sleep(delay).await;

    send_protocol_event_direct(
        factory,
        port,
        baud,
        "claude.approval.dismiss",
        json!({
            "id": &scenario.approval_id,
        }),
    )?;

    Ok(())
}

async fn wait_for_approval_dismissal(
    client: &Client,
    base_url: &str,
    approval_id: &str,
) -> Result<()> {
    let poll_interval = std::time::Duration::from_millis(100);
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(5);

    while std::time::Instant::now() <= deadline {
        let response = client
            .get(format!("{base_url}/v1/claude/approvals/{approval_id}"))
            .timeout(std::time::Duration::from_secs(2))
            .send()
            .await
            .with_context(|| format!("failed to poll approval {approval_id} from local agent"))?;

        if response.status().is_success() {
            let body: Value = response.json().await.unwrap_or_default();
            let approval = body.get("approval").unwrap_or(&Value::Null);
            if approval.get("dismissed").and_then(|value| value.as_bool()).unwrap_or(false) {
                return Ok(());
            }
            if approval.get("decision").and_then(|value| value.as_str()).is_some() {
                return Err(anyhow!(
                    "approval {approval_id} was resolved on device before host dismiss test completed"
                ));
            }
        }

        tokio::time::sleep(poll_interval).await;
    }

    Err(anyhow!("approval {approval_id} was not dismissed by the local agent within 5s"))
}

async fn ingest_from_stdin() -> Result<()> {
    let mut stdin = String::new();
    io::stdin().read_to_string(&mut stdin).context("failed to read stdin")?;

    let raw: RawHookInput = match serde_json::from_str(&stdin) {
        Ok(raw) => raw,
        Err(err) => {
            debug!("ignoring invalid hook JSON: {err}");
            return Ok(());
        }
    };

    let event = sanitize_raw_event(raw);
    let client = Client::new();
    let response = client
        .post(format!("{}/v1/claude/events", admin_base_url()))
        .json(&event)
        .timeout(std::time::Duration::from_millis(100))
        .send()
        .await;

    if let Err(err) = response {
        warn!("agent unavailable for claude ingest: {err}");
    }

    Ok(())
}

async fn agent_status() -> Result<()> {
    let client = Client::new();
    let response = client
        .get(format!("{}/v1/agent/status", admin_base_url()))
        .send()
        .await
        .context("failed to contact local esp32dash agent")?
        .error_for_status()
        .context("agent returned non-success status")?;

    let body = response.text().await?;
    println!("{body}");
    Ok(())
}

async fn rpc_via_agent(request: RpcRequest) -> Result<Value> {
    let client = Client::new();
    let response = client
        .post(format!("{}/v1/device/rpc", admin_base_url()))
        .json(&request)
        .send()
        .await
        .context("failed to contact local esp32dash agent")?;

    if !response.status().is_success() {
        let status = response.status();
        let body = response.text().await.unwrap_or_default();
        if let Ok(err) = serde_json::from_str::<AdminErrorResponse>(&body) {
            return Err(anyhow!(err.error));
        }
        return Err(anyhow!("agent rpc failed with {status}: {body}"));
    }

    let rpc: AdminRpcResponse = response.json().await?;
    if !rpc.ok {
        return Err(anyhow!(
            "{}",
            rpc.error.unwrap_or_else(|| "agent rpc failed without an error body".to_string())
        ));
    }

    rpc.result.context("agent rpc succeeded but did not include a result payload")
}

fn install_launchd() -> Result<()> {
    let executable = env::current_exe().context("failed to resolve current executable")?;
    let plist_path = launchd::install_launchd(&executable)?;
    println!("{{\"ok\":true,\"plist_path\":\"{}\"}}", json_escape_path(plist_path));
    Ok(())
}

fn uninstall_launchd() -> Result<()> {
    match launchd::uninstall_launchd()? {
        Some(path) => println!("{{\"ok\":true,\"removed\":\"{}\"}}", json_escape_path(path)),
        None => println!("{{\"ok\":true,\"removed\":null}}"),
    }
    Ok(())
}

fn install_hooks(args: InstallHooksArgs) -> Result<()> {
    let executable = env::current_exe().context("failed to resolve current executable")?;
    let executable = executable.canonicalize().unwrap_or(executable);
    let result: InstallHooksResult = hooks::install_hooks(&executable, args.force)?;
    print_json(&result)
}

fn sanitize_raw_event(raw: RawHookInput) -> LocalHookEvent {
    let waiting_prompt = extract_waiting_prompt(&raw);
    let message = raw.message.or(raw.reason).and_then(|message| sanitize_message(&message));
    let prompt_raw = raw.prompt.filter(|prompt| !prompt.trim().is_empty());
    let prompt_preview = prompt_raw.as_deref().and_then(sanitize_prompt_preview);
    let permission_mode = raw
        .permission_mode
        .as_deref()
        .map(sanitize_permission_mode)
        .filter(|mode| !mode.is_empty())
        .unwrap_or_else(|| "default".into());

    LocalHookEvent {
        session_id: raw.session_id,
        cwd: raw.cwd,
        hook_event_name: raw.hook_event_name,
        message,
        prompt_preview,
        prompt_raw,
        tool_name: raw.tool_name.and_then(|tool| sanitize_tool_name(&tool)),
        tool_use_id: raw.tool_use_id,
        permission_mode,
        waiting_prompt,
        recv_ts: now_epoch(),
        claude_pid: hook_claude_pid(),
    }
}

fn extract_waiting_prompt(raw: &RawHookInput) -> Option<WaitingPrompt> {
    match raw.hook_event_name.as_str() {
        "PreToolUse" if raw.tool_name.as_deref() == Some("AskUserQuestion") => {
            raw.tool_input.as_ref().and_then(parse_ask_user_question_prompt)
        }
        "Elicitation" => parse_elicitation_prompt(raw),
        _ => None,
    }
}

fn parse_ask_user_question_prompt(tool_input: &Value) -> Option<WaitingPrompt> {
    let questions = tool_input.get("questions")?.as_array()?;
    let first = questions.first()?;
    let question = value_string(first, &["question"]).and_then(sanitize_waiting_detail)?;
    let question_count = questions.iter().filter(|item| item.is_object()).count().max(1);
    let default_title = if question_count > 1 {
        format!("Question 1/{question_count}")
    } else {
        "Question".to_string()
    };
    let title = value_string(first, &["header"])
        .and_then(sanitize_waiting_title)
        .unwrap_or_else(|| sanitize_snapshot_title(&default_title));
    let options = first.get("options").and_then(parse_waiting_options).unwrap_or_default();

    Some(WaitingPrompt {
        kind: WaitingPromptKind::AskUserQuestion,
        title,
        question,
        options,
    })
}

fn parse_elicitation_prompt(raw: &RawHookInput) -> Option<WaitingPrompt> {
    let title = value_string_map(
        &raw.extra,
        &["title", "header", "label", "server_name", "server", "mcp_server"],
    )
    .and_then(sanitize_waiting_title)
    .unwrap_or_else(|| "Awaiting input".into());
    let question = value_string_map(&raw.extra, &["question", "prompt", "message"])
        .or_else(|| raw.message.as_deref())
        .or_else(|| raw.reason.as_deref())
        .and_then(sanitize_waiting_detail)?;
    let options = raw
        .extra
        .get("options")
        .and_then(parse_waiting_options)
        .or_else(|| raw.extra.get("choices").and_then(parse_waiting_options))
        .unwrap_or_default();

    Some(WaitingPrompt {
        kind: WaitingPromptKind::Elicitation,
        title,
        question,
        options,
    })
}

fn parse_waiting_options(value: &Value) -> Option<Vec<WaitingPromptOption>> {
    let options: Vec<WaitingPromptOption> =
        value.as_array()?.iter().filter_map(parse_waiting_option).take(4).collect();
    (!options.is_empty()).then_some(options)
}

fn parse_waiting_option(value: &Value) -> Option<WaitingPromptOption> {
    match value {
        Value::String(label) => Some(WaitingPromptOption {
            label: sanitize_waiting_detail(label)?,
            description: None,
        }),
        Value::Object(map) => {
            let label = ["label", "title", "value", "name"]
                .iter()
                .find_map(|key| map.get(*key).and_then(Value::as_str))
                .and_then(sanitize_waiting_detail)?;
            let description = ["description", "detail", "hint", "explanation"]
                .iter()
                .find_map(|key| map.get(*key).and_then(Value::as_str))
                .and_then(sanitize_waiting_detail);
            Some(WaitingPromptOption {
                label,
                description,
            })
        }
        _ => None,
    }
}

fn value_string<'a>(value: &'a Value, keys: &[&str]) -> Option<&'a str> {
    keys.iter()
        .find_map(|key| value.get(*key).and_then(Value::as_str))
        .filter(|value| !value.trim().is_empty())
}

fn value_string_map<'a>(
    values: &'a std::collections::BTreeMap<String, Value>,
    keys: &[&str],
) -> Option<&'a str> {
    keys.iter()
        .find_map(|key| values.get(*key).and_then(Value::as_str))
        .filter(|value| !value.trim().is_empty())
}

fn sanitize_waiting_title(input: &str) -> Option<String> {
    let title = sanitize_snapshot_title(input);
    (!title.is_empty()).then_some(title)
}

fn sanitize_waiting_detail(input: &str) -> Option<String> {
    let detail = sanitize_snapshot_detail(input);
    (!detail.is_empty()).then_some(detail)
}

fn hook_claude_pid() -> Option<u32> {
    env::var("ESP32DASH_CLAUDE_PID")
        .ok()
        .and_then(|value| value.trim().parse::<u32>().ok())
        .filter(|pid| *pid > 0)
}

fn now_epoch() -> u64 {
    std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap_or_default().as_secs()
}

fn now_epoch_millis() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis() as u64
}

fn admin_base_url() -> String {
    format!("http://{}", compat::admin_addr())
}

fn print_json<T: Serialize>(value: &T) -> Result<()> {
    println!("{}", serde_json::to_string_pretty(value)?);
    Ok(())
}

fn write_screenshot_png(path: &PathBuf, screenshot: &DeviceScreenshot) -> Result<()> {
    let width = screenshot.meta.width as usize;
    let height = screenshot.meta.height as usize;
    let stride = screenshot.meta.stride_bytes as usize;
    let row_bytes = width * 2;
    let expected_min = stride * height;
    let mut rgb = vec![0u8; width * height * 3];

    if screenshot.meta.format != "rgb565_le" {
        return Err(anyhow!("unsupported screenshot format {}", screenshot.meta.format));
    }
    if stride < row_bytes {
        return Err(anyhow!("invalid screenshot stride {} for width {}", stride, width));
    }
    if screenshot.data.len() < expected_min {
        return Err(anyhow!(
            "screenshot data too short: {} < {}",
            screenshot.data.len(),
            expected_min
        ));
    }

    for y in 0..height {
        let src_row = &screenshot.data[y * stride..y * stride + row_bytes];
        let dst_row = &mut rgb[y * width * 3..(y + 1) * width * 3];

        for x in 0..width {
            let pixel = u16::from_le_bytes([src_row[x * 2], src_row[x * 2 + 1]]);
            let red = ((pixel >> 11) & 0x1F) as u8;
            let green = ((pixel >> 5) & 0x3F) as u8;
            let blue = (pixel & 0x1F) as u8;

            dst_row[x * 3] = (u16::from(red) * 255 / 31) as u8;
            dst_row[x * 3 + 1] = (u16::from(green) * 255 / 63) as u8;
            dst_row[x * 3 + 2] = (u16::from(blue) * 255 / 31) as u8;
        }
    }

    if let Some(parent) = path.parent().filter(|parent| !parent.as_os_str().is_empty()) {
        fs::create_dir_all(parent).with_context(|| {
            format!("failed to create screenshot directory {}", parent.display())
        })?;
    }

    let file =
        File::create(path).with_context(|| format!("failed to create {}", path.display()))?;
    let writer = BufWriter::new(file);
    let mut encoder =
        png::Encoder::new(writer, screenshot.meta.width.into(), screenshot.meta.height.into());
    encoder.set_color(png::ColorType::Rgb);
    encoder.set_depth(png::BitDepth::Eight);
    let mut png_writer = encoder.write_header().context("failed to write PNG header")?;
    png_writer.write_image_data(&rgb).context("failed to write PNG image data")?;
    Ok(())
}

fn json_escape_path(path: PathBuf) -> String {
    path.to_string_lossy().replace('\\', "\\\\").replace('"', "\\\"")
}

async fn respond_from_stdin() -> Result<()> {
    let mut stdin = String::new();
    io::stdin().read_to_string(&mut stdin).context("failed to read stdin")?;

    let raw: Value = match serde_json::from_str(&stdin) {
        Ok(v) => v,
        Err(err) => {
            debug!("ignoring invalid hook JSON: {err}");
            return Ok(());
        }
    };

    let event_name = raw
        .get("hook_event_name")
        .and_then(|v| v.as_str())
        .unwrap_or("unknown");

    match event_name {
        "PermissionRequest" => handle_permission_request(&stdin, &raw).await,
        "Elicitation" => handle_elicitation(&stdin, &raw).await,
        "PreToolUse" => handle_ask_user_question(&stdin, &raw).await,
        _ => {
            debug!("respond: unknown event type {event_name}, falling through");
            Ok(())
        }
    }
}

async fn handle_permission_request(stdin: &str, raw: &Value) -> Result<()> {
    let tool_name = raw.get("tool_name").and_then(|v| v.as_str()).unwrap_or("unknown").to_string();
    let display_tool_name = sanitize_tool_name(&tool_name).unwrap_or_else(|| "unknown".into());

    let tool_input_summary = summarize_tool_input(raw);

    let permission_mode = raw
        .get("permission_mode")
        .and_then(|v| v.as_str())
        .map(sanitize_permission_mode)
        .filter(|mode| !mode.is_empty())
        .unwrap_or_else(|| "default".into());

    let approval_id = next_approval_id();
    let event = serde_json::from_str::<RawHookInput>(stdin).ok().map(sanitize_raw_event);

    // Also ingest as a normal event so the dashboard updates
    if let Some(event) = event.as_ref() {
        let client = Client::new();
        let _ = client
            .post(format!("{}/v1/claude/events", admin_base_url()))
            .json(&event)
            .timeout(std::time::Duration::from_millis(100))
            .send()
            .await;
    }

    let permission_suggestions = raw
        .get("permission_suggestions")
        .and_then(|v| v.as_array())
        .cloned()
        .unwrap_or_default();

    // Submit approval to agent
    let client = Client::new();
    let submit_response = client
        .post(format!("{}/v1/claude/approvals", admin_base_url()))
        .json(&json!({
            "id": approval_id,
            "tool_name": display_tool_name,
            "tool_input_summary": tool_input_summary,
            "permission_mode": permission_mode,
            "session_id": event.as_ref().map(|value| value.session_id.as_str()).unwrap_or_default(),
            "tool_use_id": event.as_ref().and_then(|value| value.tool_use_id.as_deref()),
            "permission_suggestions": permission_suggestions,
        }))
        .timeout(std::time::Duration::from_secs(5))
        .send()
        .await;

    match submit_response {
        Ok(response) if response.status().is_success() => {}
        Ok(response) => {
            warn!("approval submit failed with status {}", response.status());
            return Ok(());
        }
        Err(err) => {
            warn!("agent unavailable for approval: {err}");
            // Agent not running — let Claude Code show its normal permission dialog
            return Ok(());
        }
    }

    // Poll for decision
    let poll_interval = std::time::Duration::from_millis(500);
    let app_config = AppConfig::load().unwrap_or_default();
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(app_config.approval.timeout_secs);

    loop {
        if std::time::Instant::now() > deadline {
            warn!("approval timed out, denying");
            let output = permission_request_output_for_decision(
                "deny",
                &tool_name,
                Some("Approval timed out on ESP32 dashboard"),
                &[],
            );
            println!("{}", serde_json::to_string(&output)?);
            return Ok(());
        }

        tokio::time::sleep(poll_interval).await;

        let response = client
            .get(format!("{}/v1/claude/approvals/{}", admin_base_url(), approval_id))
            .timeout(std::time::Duration::from_secs(2))
            .send()
            .await;

        let body: Value = match response {
            Ok(resp) if resp.status().is_success() => resp.json().await.unwrap_or_default(),
            _ => continue,
        };

        let approval = match body.get("approval") {
            Some(a) => a,
            None => continue,
        };

        let resolved = approval.get("resolved").and_then(|v| v.as_bool()).unwrap_or(false);
        if !resolved {
            continue;
        }

        let dismissed = approval.get("dismissed").and_then(|v| v.as_bool()).unwrap_or(false);
        if dismissed {
            return Ok(());
        }

        let decision = approval.get("decision").and_then(|v| v.as_str()).unwrap_or("deny");
        let permission_suggestions = approval
            .get("permission_suggestions")
            .and_then(|v| v.as_array())
            .cloned()
            .unwrap_or_default();
        let output = permission_request_output_for_decision(
            decision,
            &tool_name,
            Some("Denied from ESP32 dashboard"),
            &permission_suggestions,
        );

        println!("{}", serde_json::to_string(&output)?);
        return Ok(());
    }
}

async fn handle_elicitation(stdin: &str, raw: &Value) -> Result<()> {
    let prompt = parse_elicitation_prompt_from_raw(raw);
    let app_config = AppConfig::load().unwrap_or_default();

    // Check for form mode (complex schema that can't be rendered on device)
    // Decline any schema with type: object — catches allOf, anyOf, oneOf compositions too
    let is_form_mode = raw
        .get("requested_schema")
        .and_then(|v| v.get("type"))
        .and_then(Value::as_str)
        == Some("object");

    if is_form_mode {
        let output = json!({
            "hookSpecificOutput": {
                "hookEventName": "Elicitation",
                "decision": {
                    "behavior": "decline",
                    "message": "Complex form input not supported on ESP32 dashboard. Please answer in the terminal."
                }
            }
        });
        println!("{}", serde_json::to_string(&output)?);
        return Ok(());
    }

    let Some(prompt) = prompt else {
        // Empty options or failed to parse — show hint directing user to terminal
        let output = json!({
            "hookSpecificOutput": {
                "hookEventName": "Elicitation",
                "decision": {
                    "behavior": "decline",
                    "message": "This prompt requires terminal input. Please answer in the terminal."
                }
            }
        });
        println!("{}", serde_json::to_string(&output)?);
        return Ok(());
    };

    let prompt_id = next_prompt_id();
    let event = serde_json::from_str::<RawHookInput>(stdin).ok().map(sanitize_raw_event);

    // Also ingest as a normal event so the dashboard updates
    if let Some(event) = event.as_ref() {
        let client = Client::new();
        let _ = client
            .post(format!("{}/v1/claude/events", admin_base_url()))
            .json(&event)
            .timeout(std::time::Duration::from_millis(100))
            .send()
            .await;
    }

    // Submit prompt to agent
    let client = Client::new();
    let submit_response = client
        .post(format!("{}/v1/claude/prompts", admin_base_url()))
        .json(&json!({
            "id": prompt_id,
            "kind": "elicitation",
            "title": prompt.title,
            "question": prompt.question,
            "options": prompt.options,
            "session_id": event.as_ref().map(|e| e.session_id.as_str()).unwrap_or_default(),
            "tool_use_id": event.as_ref().and_then(|e| e.tool_use_id.as_deref()),
        }))
        .timeout(std::time::Duration::from_secs(5))
        .send()
        .await;

    match submit_response {
        Ok(response) if response.status().is_success() => {}
        Ok(response) => {
            warn!("prompt submit failed with status {}", response.status());
            return Ok(());
        }
        Err(err) => {
            warn!("agent unavailable for prompt: {err}");
            return Ok(());
        }
    }

    // Poll for selection
    let poll_interval = std::time::Duration::from_millis(500);
    let deadline = std::time::Instant::now()
        + std::time::Duration::from_secs(app_config.approval.elicitation_timeout_secs);

    loop {
        if std::time::Instant::now() > deadline {
            warn!("elicitation timed out");
            let output = json!({
                "hookSpecificOutput": {
                    "hookEventName": "Elicitation",
                    "decision": {
                        "behavior": "decline",
                        "message": "Elicitation timed out on ESP32 dashboard"
                    }
                }
            });
            println!("{}", serde_json::to_string(&output)?);
            return Ok(());
        }

        tokio::time::sleep(poll_interval).await;

        let response = client
            .get(format!("{}/v1/claude/prompts/{}", admin_base_url(), prompt_id))
            .timeout(std::time::Duration::from_secs(2))
            .send()
            .await;

        let body: Value = match response {
            Ok(resp) if resp.status().is_success() => resp.json().await.unwrap_or_default(),
            _ => continue,
        };

        let prompt_status = match body.get("prompt") {
            Some(p) => p,
            None => continue,
        };

        let resolved = prompt_status.get("resolved").and_then(|v| v.as_bool()).unwrap_or(false);
        if !resolved {
            continue;
        }

        let selection_index = prompt_status
            .get("selection_index")
            .and_then(|v| v.as_u64())
            .map(|v| v as usize);

        let output = match selection_index {
            Some(index) => {
                let selected_label = prompt
                    .options
                    .get(index)
                    .map(|o| o.label.clone())
                    .unwrap_or_default();
                json!({
                    "hookSpecificOutput": {
                        "hookEventName": "Elicitation",
                        "decision": {
                            "behavior": "accept",
                            "updatedInput": {
                                "value": selected_label
                            }
                        }
                    }
                })
            }
            None => json!({
                "hookSpecificOutput": {
                    "hookEventName": "Elicitation",
                    "decision": {
                        "behavior": "decline",
                        "message": "Declined from ESP32 dashboard"
                    }
                }
            }),
        };

        println!("{}", serde_json::to_string(&output)?);
        return Ok(());
    }
}

async fn handle_ask_user_question(stdin: &str, raw: &Value) -> Result<()> {
    let event = serde_json::from_str::<RawHookInput>(stdin).ok();
    let tool_input = raw.get("tool_input");
    let prompt = tool_input.and_then(parse_ask_user_question_prompt);

    let Some(prompt) = prompt else {
        warn!("AskUserQuestion: failed to parse prompt");
        return Ok(());
    };

    let prompt_id = next_prompt_id();
    let app_config = AppConfig::load().unwrap_or_default();

    // Also ingest as a normal event so the dashboard updates
    if let Some(event) = event.as_ref() {
        let sanitized = sanitize_raw_event(event.clone());
        let client = Client::new();
        let _ = client
            .post(format!("{}/v1/claude/events", admin_base_url()))
            .json(&sanitized)
            .timeout(std::time::Duration::from_millis(100))
            .send()
            .await;
    }

    // Submit prompt to agent
    let client = Client::new();
    let submit_response = client
        .post(format!("{}/v1/claude/prompts", admin_base_url()))
        .json(&json!({
            "id": prompt_id,
            "kind": "ask_user_question",
            "title": prompt.title,
            "question": prompt.question,
            "options": prompt.options,
            "session_id": event.as_ref().map(|e| e.session_id.as_str()).unwrap_or_default(),
            "tool_use_id": event.as_ref().and_then(|e| e.tool_use_id.as_deref()),
        }))
        .timeout(std::time::Duration::from_secs(5))
        .send()
        .await;

    match submit_response {
        Ok(response) if response.status().is_success() => {}
        Ok(response) => {
            warn!("prompt submit failed with status {}", response.status());
            return Ok(());
        }
        Err(err) => {
            warn!("agent unavailable for prompt: {err}");
            return Ok(());
        }
    }

    // Poll for selection
    let poll_interval = std::time::Duration::from_millis(500);
    let deadline = std::time::Instant::now()
        + std::time::Duration::from_secs(app_config.approval.question_timeout_secs);

    loop {
        if std::time::Instant::now() > deadline {
            warn!("ask user question timed out");
            let output = json!({
                "hookSpecificOutput": {
                    "hookEventName": "PreToolUse",
                    "decision": {
                        "behavior": "deny",
                        "message": "Question timed out on ESP32 dashboard"
                    }
                }
            });
            println!("{}", serde_json::to_string(&output)?);
            return Ok(());
        }

        tokio::time::sleep(poll_interval).await;

        let response = client
            .get(format!("{}/v1/claude/prompts/{}", admin_base_url(), prompt_id))
            .timeout(std::time::Duration::from_secs(2))
            .send()
            .await;

        let body: Value = match response {
            Ok(resp) if resp.status().is_success() => resp.json().await.unwrap_or_default(),
            _ => continue,
        };

        let prompt_status = match body.get("prompt") {
            Some(p) => p,
            None => continue,
        };

        let resolved = prompt_status.get("resolved").and_then(|v| v.as_bool()).unwrap_or(false);
        if !resolved {
            continue;
        }

        let selection_index = prompt_status
            .get("selection_index")
            .and_then(|v| v.as_u64())
            .map(|v| v as usize);

        let output = match selection_index {
            Some(index) => {
                let selected_label = prompt
                    .options
                    .get(index)
                    .map(|o| o.label.clone())
                    .unwrap_or_default();
                json!({
                    "hookSpecificOutput": {
                        "hookEventName": "PreToolUse",
                        "decision": {
                            "behavior": "allow",
                            "updatedInput": {
                                "answers": [{ "question": prompt.question, "answer": selected_label }]
                            }
                        }
                    }
                })
            }
            None => json!({
                "hookSpecificOutput": {
                    "hookEventName": "PreToolUse",
                    "decision": {
                        "behavior": "deny",
                        "message": "Declined from ESP32 dashboard"
                    }
                }
            }),
        };

        println!("{}", serde_json::to_string(&output)?);
        return Ok(());
    }
}

fn next_approval_id() -> String {
    let sequence = NEXT_APPROVAL_ID_SEQ.fetch_add(1, Ordering::Relaxed);
    build_approval_id(now_epoch_millis(), sequence)
}

fn next_prompt_id() -> String {
    let sequence = NEXT_PROMPT_ID_SEQ.fetch_add(1, Ordering::Relaxed);
    format!("prompt-{}", build_approval_id(now_epoch_millis(), sequence))
}

fn parse_elicitation_prompt_from_raw(raw: &Value) -> Option<WaitingPrompt> {
    let title = raw
        .get("title")
        .or_else(|| raw.get("header"))
        .or_else(|| raw.get("label"))
        .and_then(Value::as_str)
        .and_then(sanitize_waiting_title)
        .unwrap_or_else(|| "Awaiting input".into());
    let question = raw
        .get("question")
        .or_else(|| raw.get("prompt"))
        .or_else(|| raw.get("message"))
        .and_then(Value::as_str)
        .and_then(sanitize_waiting_detail)?;
    let options = raw
        .get("options")
        .and_then(parse_waiting_options)
        .or_else(|| raw.get("choices").and_then(parse_waiting_options))
        .unwrap_or_default();

    if options.is_empty() {
        return None;
    }

    Some(WaitingPrompt {
        kind: WaitingPromptKind::Elicitation,
        title,
        question,
        options,
    })
}

fn permission_request_output_for_decision(
    decision: &str,
    tool_name: &str,
    deny_message: Option<&str>,
    permission_suggestions: &[Value],
) -> Value {
    match decision {
        "allow" => json!({
            "hookSpecificOutput": {
                "hookEventName": "PermissionRequest",
                "decision": {
                    "behavior": "allow"
                }
            }
        }),
        "allow_always" | "yolo" => {
            let updated_permissions = if permission_suggestions.is_empty() {
                json!([{
                    "type": "addRules",
                    "rules": [{ "toolName": tool_name }],
                    "behavior": "allow",
                    "destination": "localSettings"
                }])
            } else {
                json!(permission_suggestions)
            };
            json!({
                "hookSpecificOutput": {
                    "hookEventName": "PermissionRequest",
                    "decision": {
                        "behavior": "allow",
                        "updatedPermissions": updated_permissions
                    }
                }
            })
        }
        _ => json!({
            "hookSpecificOutput": {
                "hookEventName": "PermissionRequest",
                "decision": {
                    "behavior": "deny",
                    "message": deny_message.unwrap_or("Denied from ESP32 dashboard")
                }
            }
        }),
    }
}

fn summarize_tool_input(raw: &Value) -> String {
    let tool_input = match raw.get("tool_input") {
        Some(input) => input,
        None => return String::new(),
    };

    // For Bash, show the command
    if let Some(cmd) = tool_input.get("command").and_then(|v| v.as_str()) {
        return sanitize_approval_summary(cmd);
    }
    // For Edit/Write, show the file path
    if let Some(path) = tool_input.get("file_path").and_then(|v| v.as_str()) {
        return sanitize_approval_summary(path);
    }
    // For other tools, compact JSON
    match serde_json::to_string(tool_input) {
        Ok(s) => sanitize_approval_summary(&s),
        Err(_) => String::new(),
    }
}

fn init_tracing() {
    let filter = EnvFilter::try_from_default_env().unwrap_or_else(|_| EnvFilter::new("info"));
    let _ = fmt().with_env_filter(filter).with_target(false).try_init();
}

#[cfg(test)]
mod tests {
    use std::sync::Arc;

    use anyhow::Result;
    use axum::{
        Json, Router,
        extract::{Path, State},
        http::StatusCode,
        routing::{get, post},
    };
    use tokio::{net::TcpListener, sync::Mutex};

    use super::*;

    #[derive(Debug, Clone)]
    struct RecordedRequest {
        path: String,
        body: Value,
    }

    #[derive(Clone, Default)]
    struct FakeAgentState {
        requests: Arc<Mutex<Vec<RecordedRequest>>>,
        dismissed: Arc<Mutex<bool>>,
    }

    async fn record_event(
        State(state): State<FakeAgentState>,
        Json(body): Json<Value>,
    ) -> Json<Value> {
        state.requests.lock().await.push(RecordedRequest {
            path: "/v1/claude/events".into(),
            body: body.clone(),
        });
        if body.get("hook_event_name").and_then(|value| value.as_str()) == Some("PreToolUse") {
            *state.dismissed.lock().await = true;
        }
        Json(json!({ "ok": true }))
    }

    async fn record_approval(
        State(state): State<FakeAgentState>,
        Json(body): Json<Value>,
    ) -> (StatusCode, Json<Value>) {
        state.requests.lock().await.push(RecordedRequest {
            path: "/v1/claude/approvals".into(),
            body,
        });
        (StatusCode::CREATED, Json(json!({ "ok": true })))
    }

    async fn get_approval(
        State(state): State<FakeAgentState>,
        Path(id): Path<String>,
    ) -> Json<Value> {
        let dismissed = *state.dismissed.lock().await;
        Json(json!({
            "ok": true,
            "approval": {
                "id": id,
                "resolved": dismissed,
                "dismissed": dismissed,
                "timed_out": false,
                "decision": Value::Null,
                "tool_name": "Bash",
                "tool_input_summary": "rm -rf /tmp/test",
            }
        }))
    }

    #[tokio::test]
    async fn local_agent_available_detects_running_agent() -> Result<()> {
        let router =
            Router::new().route("/v1/agent/status", get(|| async { Json(json!({ "ok": true })) }));
        let listener = TcpListener::bind("127.0.0.1:0").await?;
        let addr = listener.local_addr()?;
        let server = tokio::spawn(async move { axum::serve(listener, router).await });

        let available = local_agent_available(&Client::new(), &format!("http://{addr}")).await;

        server.abort();
        assert!(available);
        Ok(())
    }

    #[tokio::test]
    async fn approve_dismiss_scenario_uses_agent_event_flow() -> Result<()> {
        let state = FakeAgentState::default();
        let router = Router::new()
            .route("/v1/claude/events", post(record_event))
            .route("/v1/claude/approvals", post(record_approval))
            .route("/v1/claude/approvals/{id}", get(get_approval))
            .with_state(state.clone());
        let listener = TcpListener::bind("127.0.0.1:0").await?;
        let addr = listener.local_addr()?;
        let server = tokio::spawn(async move { axum::serve(listener, router).await });

        let scenario = build_approve_dismiss_scenario("Bash", "rm -rf /tmp/test");
        run_approve_dismiss_scenario(
            &Client::new(),
            &format!("http://{addr}"),
            &scenario,
            std::time::Duration::ZERO,
        )
        .await?;

        let requests = state.requests.lock().await;
        assert_eq!(requests.len(), 3);
        assert_eq!(requests[0].path, "/v1/claude/events");
        assert_eq!(requests[0].body["hook_event_name"], "PermissionRequest");
        assert_eq!(requests[1].path, "/v1/claude/approvals");
        assert_eq!(requests[1].body["id"], scenario.approval_id);
        assert_eq!(requests[1].body["session_id"], scenario.request_event.session_id);
        assert_eq!(
            requests[1].body["tool_use_id"],
            scenario.request_event.tool_use_id.as_deref().unwrap_or_default()
        );
        assert_eq!(requests[2].path, "/v1/claude/events");
        assert_eq!(requests[2].body["hook_event_name"], "PreToolUse");
        assert_eq!(requests[2].body["session_id"], scenario.clear_event.session_id);
        assert_eq!(
            requests[2].body["tool_use_id"],
            scenario.clear_event.tool_use_id.as_deref().unwrap_or_default()
        );

        server.abort();
        Ok(())
    }

    #[test]
    fn allow_always_output_adds_persistent_allow_rule_for_tool() {
        let output = permission_request_output_for_decision("allow_always", "Bash", None, &[]);
        let decision = &output["hookSpecificOutput"]["decision"];

        assert_eq!(decision["behavior"], "allow");
        assert_eq!(decision["updatedPermissions"][0]["type"], "addRules");
        assert_eq!(decision["updatedPermissions"][0]["rules"][0]["toolName"], "Bash");
        assert_eq!(decision["updatedPermissions"][0]["behavior"], "allow");
        assert_eq!(decision["updatedPermissions"][0]["destination"], "localSettings");
    }

    #[test]
    fn sanitize_raw_event_preserves_ask_user_question_prompt() {
        let event = sanitize_raw_event(RawHookInput {
            session_id: "sess-1".into(),
            cwd: "/tmp/project".into(),
            hook_event_name: "PreToolUse".into(),
            message: None,
            prompt: None,
            tool_name: Some("AskUserQuestion".into()),
            tool_use_id: Some("tool-1".into()),
            permission_mode: Some("default".into()),
            reason: None,
            tool_input: Some(json!({
                "questions": [{
                    "header": "Plan ready",
                    "question": "Execute now or revise?",
                    "options": [
                        { "label": "Execute", "description": "Run the plan" },
                        { "label": "More prompt", "description": "Edit the plan" }
                    ]
                }]
            })),
            extra: Default::default(),
        });

        let prompt = event.waiting_prompt.expect("waiting prompt should be preserved");
        assert_eq!(prompt.kind, WaitingPromptKind::AskUserQuestion);
        assert_eq!(prompt.title, "Plan ready");
        assert_eq!(prompt.question, "Execute now or revise?");
        assert_eq!(prompt.options.len(), 2);
        assert_eq!(prompt.options[0].label, "Execute");
        assert_eq!(prompt.options[0].description.as_deref(), Some("Run the plan"));
    }

    #[test]
    fn sanitize_raw_event_preserves_elicitation_prompt() {
        let mut extra = std::collections::BTreeMap::new();
        extra.insert("server_name".into(), json!("notion"));
        extra.insert("prompt".into(), json!("Choose a workspace"));
        extra.insert(
            "options".into(),
            json!(["Engineering", { "label": "Design", "description": "Review assets" }]),
        );

        let event = sanitize_raw_event(RawHookInput {
            session_id: "sess-2".into(),
            cwd: "/tmp/project".into(),
            hook_event_name: "Elicitation".into(),
            message: Some("Choose a workspace".into()),
            prompt: None,
            tool_name: None,
            tool_use_id: None,
            permission_mode: Some("default".into()),
            reason: None,
            tool_input: None,
            extra,
        });

        let prompt = event.waiting_prompt.expect("elicitation prompt should be preserved");
        assert_eq!(prompt.kind, WaitingPromptKind::Elicitation);
        assert_eq!(prompt.title, "notion");
        assert_eq!(prompt.question, "Choose a workspace");
        assert_eq!(prompt.options.len(), 2);
        assert_eq!(prompt.options[0].label, "Engineering");
        assert_eq!(prompt.options[1].description.as_deref(), Some("Review assets"));
    }

    #[test]
    fn sanitize_raw_event_preserves_prompt_raw_for_host_analysis() {
        let event = sanitize_raw_event(RawHookInput {
            session_id: "sess-raw".into(),
            cwd: "/tmp/project".into(),
            hook_event_name: "UserPromptSubmit".into(),
            message: None,
            prompt: Some("first line\nsecond line".into()),
            tool_name: None,
            tool_use_id: None,
            permission_mode: Some("default".into()),
            reason: None,
            tool_input: None,
            extra: Default::default(),
        });

        assert_eq!(event.prompt_raw.as_deref(), Some("first line\nsecond line"));
        assert_eq!(event.prompt_preview.as_deref(), Some("first line"));
    }

    #[test]
    fn build_test_snapshot_preserves_requested_emotion() {
        let snapshot = build_test_snapshot(
            &ChibiState::Working,
            &ChibiEmotion::Sad,
            Some("Need help"),
            &ChibiSnapshotOptions::default(),
        );

        assert_eq!(snapshot.status, "processing");
        assert_eq!(snapshot.emotion, "sad");
        assert_eq!(snapshot.detail, "Need help");
        assert!(!snapshot.unread);
    }

    #[test]
    fn build_test_snapshot_marks_waiting_as_unread() {
        let snapshot = build_test_snapshot(
            &ChibiState::Waiting,
            &ChibiEmotion::Happy,
            None,
            &ChibiSnapshotOptions::default(),
        );

        assert_eq!(snapshot.status, "waiting_for_input");
        assert_eq!(snapshot.emotion, "happy");
        assert!(snapshot.unread);
    }

    #[test]
    fn build_test_snapshot_allows_unread_override() {
        let snapshot = build_test_snapshot(
            &ChibiState::Idle,
            &ChibiEmotion::Neutral,
            None,
            &ChibiSnapshotOptions {
                unread: ChibiUnread::On,
                ..ChibiSnapshotOptions::default()
            },
        );

        assert!(snapshot.unread);
        assert_eq!(snapshot.attention, Attention::Medium);
    }

    #[test]
    fn build_test_snapshot_allows_attention_and_metadata_overrides() {
        let snapshot = build_test_snapshot(
            &ChibiState::Idle,
            &ChibiEmotion::Happy,
            Some("Heads up"),
            &ChibiSnapshotOptions {
                attention: ChibiAttention::High,
                title: "Notice".to_string(),
                source: "manual_smoke".to_string(),
                session_id: "sess-42".to_string(),
                event: "Notification".to_string(),
                ..ChibiSnapshotOptions::default()
            },
        );

        assert_eq!(snapshot.title, "Notice");
        assert_eq!(snapshot.source, "manual_smoke");
        assert_eq!(snapshot.session_id, "sess-42");
        assert_eq!(snapshot.event, "Notification");
        assert_eq!(snapshot.detail, "Heads up");
        assert_eq!(snapshot.attention, Attention::High);
    }

    #[test]
    fn deny_output_uses_supplied_message() {
        let output =
            permission_request_output_for_decision("deny", "Bash", Some("Approval timed out"), &[]);
        assert_eq!(output["hookSpecificOutput"]["decision"]["message"], "Approval timed out");
    }

    #[test]
    fn prompt_and_approval_ids_use_separate_sequences() {
        let approval1 = next_approval_id();
        let prompt1 = next_prompt_id();
        let approval2 = next_approval_id();
        let prompt2 = next_prompt_id();

        // Approval IDs should not contain "prompt-"
        assert!(!approval1.contains("prompt-"));
        assert!(!approval2.contains("prompt-"));

        // Prompt IDs should contain "prompt-"
        assert!(prompt1.starts_with("prompt-"));
        assert!(prompt2.starts_with("prompt-"));

        // Verify sequences are independent by checking that prompt IDs increment
        // without affecting approval IDs and vice versa
        let approval_seq1 = approval1.split('-').last().unwrap().parse::<u64>().unwrap();
        let approval_seq2 = approval2.split('-').last().unwrap().parse::<u64>().unwrap();
        let prompt_seq1 = prompt1.split('-').last().unwrap().parse::<u64>().unwrap();
        let prompt_seq2 = prompt2.split('-').last().unwrap().parse::<u64>().unwrap();

        // Approval sequence should increment by 1
        assert_eq!(approval_seq2, approval_seq1 + 1);
        // Prompt sequence should increment by 1
        assert_eq!(prompt_seq2, prompt_seq1 + 1);
        // They should not be interleaved (approval_seq1 and prompt_seq1 should be independent)
        // The exact values don't matter, only that each sequence increments independently
    }

    #[test]
    fn parse_elicitation_prompt_returns_none_for_empty_options() {
        let raw = json!({
            "title": "Test",
            "question": "Choose one",
            "options": []
        });
        let result = parse_elicitation_prompt_from_raw(&raw);
        assert!(result.is_none());
    }

    #[test]
    fn parse_elicitation_prompt_returns_none_when_no_options_field() {
        let raw = json!({
            "title": "Test",
            "question": "Choose one"
        });
        let result = parse_elicitation_prompt_from_raw(&raw);
        assert!(result.is_none());
    }

    #[test]
    fn parse_elicitation_prompt_succeeds_with_valid_options() {
        let raw = json!({
            "title": "Test",
            "question": "Choose one",
            "options": ["A", "B"]
        });
        let result = parse_elicitation_prompt_from_raw(&raw);
        assert!(result.is_some());
        let prompt = result.unwrap();
        assert_eq!(prompt.options.len(), 2);
        assert_eq!(prompt.options[0].label, "A");
        assert_eq!(prompt.options[1].label, "B");
    }
}
