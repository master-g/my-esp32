mod daemon;
mod launchd;
mod model;
mod normalizer;

use std::{
    env,
    io::{self, Read},
    path::PathBuf,
};

use anyhow::{Context, Result, anyhow};
use clap::{Parser, Subcommand};
use reqwest::Client;
use tracing::{debug, warn};
use tracing_subscriber::{EnvFilter, fmt};

use crate::{
    daemon::Config,
    model::{LocalHookEvent, RawHookInput},
    normalizer::sanitize_prompt_preview,
};

#[derive(Debug, Parser)]
#[command(name = "claude-bridge")]
#[command(about = "Rust daemon and CLI for Claude Code bridge events")]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Debug, Subcommand)]
enum Command {
    Daemon,
    Send {
        #[arg(long)]
        event_from_stdin: bool,
    },
    Status,
    InstallLaunchd,
    UninstallLaunchd,
}

#[tokio::main]
async fn main() -> Result<()> {
    init_tracing();
    let cli = Cli::parse();

    match cli.command {
        Command::Daemon => {
            let config = Config::from_env()?;
            daemon::run(config).await
        }
        Command::Send { event_from_stdin } => {
            if !event_from_stdin {
                return Err(anyhow!("send currently requires --event-from-stdin"));
            }
            send_from_stdin().await
        }
        Command::Status => status().await,
        Command::InstallLaunchd => install_launchd(),
        Command::UninstallLaunchd => uninstall_launchd(),
    }
}

async fn send_from_stdin() -> Result<()> {
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
    let admin_base = admin_base_url();

    let response = client
        .post(format!("{admin_base}/v1/events"))
        .json(&event)
        .timeout(std::time::Duration::from_millis(100))
        .send()
        .await;

    if let Err(err) = response {
        warn!("daemon unavailable for send: {err}");
    }

    Ok(())
}

async fn status() -> Result<()> {
    let client = Client::new();
    let response = client
        .get(format!("{}/v1/status", admin_base_url()))
        .send()
        .await
        .context("failed to contact local daemon")?
        .error_for_status()
        .context("daemon returned non-success status")?;

    let body = response.text().await?;
    println!("{body}");
    Ok(())
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
    let addr = env::var("CLAUDE_BRIDGE_ADMIN_ADDR").unwrap_or_else(|_| "127.0.0.1:37125".into());
    format!("http://{addr}")
}

fn json_escape_path(path: PathBuf) -> String {
    path.to_string_lossy()
        .replace('\\', "\\\\")
        .replace('"', "\\\"")
}

fn init_tracing() {
    let filter = EnvFilter::try_from_default_env().unwrap_or_else(|_| EnvFilter::new("info"));
    let _ = fmt().with_env_filter(filter).with_target(false).try_init();
}
