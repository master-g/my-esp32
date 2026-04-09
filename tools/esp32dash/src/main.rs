mod agent;
mod approvals;
mod compat;
mod config_ui;
mod device;
mod hooks;
mod launchd;
mod model;
mod normalizer;

use std::{
    env,
    io::{self, Read},
    path::PathBuf,
    sync::Arc,
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
    device::{UnixSerialFactory, discover_devices, open_direct_session, request_direct, request_direct_with_timeout, send_event_direct},
    hooks::InstallHooksResult,
    model::{AdminErrorResponse, AdminRpcResponse, Attention, LocalHookEvent, RawHookInput, RpcRequest, RunStatus, Snapshot, WireFrame},
    normalizer::sanitize_prompt_preview,
};

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
        about = "Handle a PermissionRequest hook event: submit to agent, wait for device approval, output decision JSON"
    )]
    Approve {
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
}

#[derive(Debug, Args)]
struct ConfigArgs {
    #[arg(
        long,
        help = "Use a specific serial port instead of autodiscovery or the running agent"
    )]
    port: Option<String>,
}

#[derive(Debug, Args)]
struct InstallHooksArgs {
    #[arg(
        short = 'f',
        long,
        help = "Overwrite and update without interactive confirmation"
    )]
    force: bool,
}

#[derive(Debug, Clone, clap::ValueEnum)]
enum ChibiState {
    Idle,
    Working,
    Waiting,
    Sleeping,
}

impl ChibiState {
    fn to_run_status(&self) -> RunStatus {
        match self {
            Self::Idle => RunStatus::Unknown,
            Self::Working => RunStatus::Processing,
            Self::Waiting => RunStatus::WaitingForInput,
            Self::Sleeping => RunStatus::Ended,
        }
    }
}

#[derive(Debug, Subcommand)]
enum ChibiCommand {
    #[command(about = "Send a specific sprite state (and optional text bubble) to the device")]
    Test {
        #[arg(long, value_enum, help = "Sprite state to display")]
        state: ChibiState,
        #[arg(long, help = "Text to show in the speech bubble")]
        bubble: Option<String>,
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
            command: ClaudeCommand::Ingest { event_from_stdin },
        } => {
            if !event_from_stdin {
                return Err(anyhow!("ingest requires --event-from-stdin"));
            }
            ingest_from_stdin().await
        }
        Command::Claude {
            command: ClaudeCommand::Approve { event_from_stdin },
        } => {
            if !event_from_stdin {
                return Err(anyhow!("approve requires --event-from-stdin"));
            }
            approve_from_stdin().await
        }
        Command::Device { command } => run_device_command(command).await,
        Command::Config(args) => config_ui::run_config_editor(args.port).await,
        Command::InstallLaunchd => install_launchd(),
        Command::UninstallLaunchd => uninstall_launchd(),
        Command::InstallHooks(args) => install_hooks(args),
        Command::Chibi { command } => run_chibi_command(command).await,
    }
}

async fn run_device_command(command: DeviceCommand) -> Result<()> {
    match command {
        DeviceCommand::List => {
            let devices = discover_devices(Arc::new(UnixSerialFactory), compat::serial_baud())?;
            print_json(&devices)
        }
        DeviceCommand::Info { port } => {
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
        DeviceCommand::Reboot { port } => {
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

    request_direct(
        Arc::new(UnixSerialFactory),
        port,
        compat::serial_baud(),
        request,
    )
}

async fn run_chibi_command(command: ChibiCommand) -> Result<()> {
    match command {
        ChibiCommand::Test {
            state,
            bubble,
            port,
        } => {
            let snapshot = build_test_snapshot(&state, bubble.as_deref());
            send_event_direct(
                Arc::new(UnixSerialFactory),
                port.as_deref(),
                compat::serial_baud(),
                &snapshot,
            )?;
            println!(
                "sent state={} bubble={:?}",
                snapshot.status,
                if snapshot.detail.is_empty() {
                    None
                } else {
                    Some(&snapshot.detail)
                }
            );
            Ok(())
        }
        ChibiCommand::Demo { port } => {
            let factory = Arc::new(UnixSerialFactory);
            let baud = compat::serial_baud();
            let mut session = open_direct_session(factory, port.as_deref(), baud)?;

            let steps: &[(ChibiState, &str)] = &[
                (ChibiState::Idle, "Just chilling..."),
                (ChibiState::Working, "Thinking hard..."),
                (ChibiState::Working, "Running tests"),
                (ChibiState::Waiting, "Need your input!"),
                (ChibiState::Sleeping, ""),
            ];

            for (i, (state, bubble)) in steps.iter().enumerate() {
                let bubble_text = if bubble.is_empty() {
                    None
                } else {
                    Some(*bubble)
                };
                let snapshot = build_test_snapshot(state, bubble_text);
                session.write_frame(&WireFrame::event(
                    "claude.update",
                    serde_json::to_value(&snapshot)?,
                ))?;
                println!(
                    "[{}/{}] state={} bubble={:?}",
                    i + 1,
                    steps.len(),
                    snapshot.status,
                    bubble_text
                );
                if i + 1 < steps.len() {
                    std::thread::sleep(std::time::Duration::from_secs(3));
                }
            }
            println!("demo complete");
            Ok(())
        }
        ChibiCommand::Approve { tool, desc, port } => {
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

            let result = request_direct_with_timeout(
                Arc::new(UnixSerialFactory),
                port.as_deref(),
                compat::serial_baud(),
                rpc,
                std::time::Duration::from_secs(300),
            )?;

            let decision = result
                .get("decision")
                .and_then(|v| v.as_str())
                .unwrap_or("unknown");
            println!("device responded: decision={decision}");
            Ok(())
        }
    }
}

fn build_test_snapshot(state: &ChibiState, bubble: Option<&str>) -> Snapshot {
    let status = state.to_run_status();
    let unread = matches!(state, ChibiState::Waiting);
    Snapshot {
        seq: 1,
        source: "chibi_test".to_string(),
        session_id: "test-session".to_string(),
        event: "Stop".to_string(),
        status: status.as_str().to_string(),
        title: "Chibi Test".to_string(),
        workspace: String::new(),
        detail: bubble.unwrap_or_default().to_string(),
        permission_mode: "default".to_string(),
        ts: now_epoch(),
        unread,
        attention: if unread {
            Attention::Medium
        } else {
            Attention::Low
        },
    }
}

async fn ingest_from_stdin() -> Result<()> {
    let mut stdin = String::new();
    io::stdin()
        .read_to_string(&mut stdin)
        .context("failed to read stdin")?;

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
            rpc.error
                .unwrap_or_else(|| "agent rpc failed without an error body".to_string())
        ));
    }

    rpc.result
        .context("agent rpc succeeded but did not include a result payload")
}

fn install_launchd() -> Result<()> {
    let executable = env::current_exe().context("failed to resolve current executable")?;
    let plist_path = launchd::install_launchd(&executable)?;
    println!(
        "{{\"ok\":true,\"plist_path\":\"{}\"}}",
        json_escape_path(plist_path)
    );
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
    let message = raw
        .message
        .or(raw.reason)
        .map(|message| trim_text(&message, 96))
        .filter(|message| !message.is_empty());

    let prompt_preview = raw.prompt.as_deref().and_then(sanitize_prompt_preview);

    LocalHookEvent {
        session_id: raw.session_id,
        cwd: raw.cwd,
        hook_event_name: raw.hook_event_name,
        message,
        prompt_preview,
        tool_name: raw
            .tool_name
            .map(|tool| trim_text(&tool, 48))
            .filter(|tool| !tool.is_empty()),
        tool_use_id: raw.tool_use_id,
        permission_mode: raw.permission_mode.unwrap_or_else(|| "default".into()),
        recv_ts: now_epoch(),
    }
}

fn trim_text(input: &str, max_chars: usize) -> String {
    input
        .lines()
        .next()
        .unwrap_or_default()
        .trim()
        .chars()
        .take(max_chars)
        .collect()
}

fn now_epoch() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs()
}

fn admin_base_url() -> String {
    format!("http://{}", compat::admin_addr())
}

fn print_json<T: Serialize>(value: &T) -> Result<()> {
    println!("{}", serde_json::to_string_pretty(value)?);
    Ok(())
}

fn json_escape_path(path: PathBuf) -> String {
    path.to_string_lossy()
        .replace('\\', "\\\\")
        .replace('"', "\\\"")
}

async fn approve_from_stdin() -> Result<()> {
    let mut stdin = String::new();
    io::stdin()
        .read_to_string(&mut stdin)
        .context("failed to read stdin")?;

    let raw: Value = match serde_json::from_str(&stdin) {
        Ok(v) => v,
        Err(err) => {
            debug!("ignoring invalid hook JSON: {err}");
            return Ok(());
        }
    };

    let tool_name = raw
        .get("tool_name")
        .and_then(|v| v.as_str())
        .unwrap_or("unknown")
        .to_string();

    let tool_input_summary = summarize_tool_input(&raw);

    let permission_mode = raw
        .get("permission_mode")
        .and_then(|v| v.as_str())
        .unwrap_or("default")
        .to_string();

    let approval_id = format!(
        "approval-{}-{}",
        now_epoch(),
        tool_name.chars().take(16).collect::<String>()
    );

    // Also ingest as a normal event so the dashboard updates
    let ingest_raw: std::result::Result<RawHookInput, _> = serde_json::from_str(&stdin);
    if let Ok(raw_event) = ingest_raw {
        let event = sanitize_raw_event(raw_event);
        let client = Client::new();
        let _ = client
            .post(format!("{}/v1/claude/events", admin_base_url()))
            .json(&event)
            .timeout(std::time::Duration::from_millis(100))
            .send()
            .await;
    }

    // Submit approval to agent
    let client = Client::new();
    let submit_response = client
        .post(format!("{}/v1/claude/approvals", admin_base_url()))
        .json(&json!({
            "id": approval_id,
            "tool_name": tool_name,
            "tool_input_summary": tool_input_summary,
            "permission_mode": permission_mode,
        }))
        .timeout(std::time::Duration::from_secs(5))
        .send()
        .await;

    if let Err(err) = submit_response {
        warn!("agent unavailable for approval: {err}");
        // Agent not running — let Claude Code show its normal permission dialog
        return Ok(());
    }

    // Poll for decision
    let poll_interval = std::time::Duration::from_millis(500);
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(300);

    loop {
        if std::time::Instant::now() > deadline {
            warn!("approval timed out, denying");
            let output = json!({
                "hookSpecificOutput": {
                    "hookEventName": "PermissionRequest",
                    "decision": {
                        "behavior": "deny",
                        "message": "Approval timed out on ESP32 dashboard"
                    }
                }
            });
            println!("{}", serde_json::to_string(&output)?);
            return Ok(());
        }

        tokio::time::sleep(poll_interval).await;

        let response = client
            .get(format!(
                "{}/v1/claude/approvals/{}",
                admin_base_url(),
                approval_id
            ))
            .timeout(std::time::Duration::from_secs(2))
            .send()
            .await;

        let body: Value = match response {
            Ok(resp) if resp.status().is_success() => {
                resp.json().await.unwrap_or_default()
            }
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

        let decision = approval
            .get("decision")
            .and_then(|v| v.as_str())
            .unwrap_or("deny");

        let output = match decision {
            "allow" => json!({
                "hookSpecificOutput": {
                    "hookEventName": "PermissionRequest",
                    "decision": {
                        "behavior": "allow"
                    }
                }
            }),
            "yolo" => json!({
                "hookSpecificOutput": {
                    "hookEventName": "PermissionRequest",
                    "decision": {
                        "behavior": "allow",
                        "updatedPermissions": [{
                            "type": "addRules",
                            "rules": [{ "toolName": tool_name }],
                            "behavior": "allow",
                            "destination": "localSettings"
                        }]
                    }
                }
            }),
            _ => json!({
                "hookSpecificOutput": {
                    "hookEventName": "PermissionRequest",
                    "decision": {
                        "behavior": "deny",
                        "message": "Denied from ESP32 dashboard"
                    }
                }
            }),
        };

        println!("{}", serde_json::to_string(&output)?);
        return Ok(());
    }
}

fn summarize_tool_input(raw: &Value) -> String {
    let tool_input = match raw.get("tool_input") {
        Some(input) => input,
        None => return String::new(),
    };

    // For Bash, show the command
    if let Some(cmd) = tool_input.get("command").and_then(|v| v.as_str()) {
        return trim_text(cmd, 80);
    }
    // For Edit/Write, show the file path
    if let Some(path) = tool_input.get("file_path").and_then(|v| v.as_str()) {
        return trim_text(path, 80);
    }
    // For other tools, compact JSON
    match serde_json::to_string(tool_input) {
        Ok(s) => trim_text(&s, 80),
        Err(_) => String::new(),
    }
}

fn init_tracing() {
    let filter = EnvFilter::try_from_default_env().unwrap_or_else(|_| EnvFilter::new("info"));
    let _ = fmt().with_env_filter(filter).with_target(false).try_init();
}
