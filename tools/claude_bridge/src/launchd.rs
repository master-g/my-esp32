use std::{
    env, fs,
    path::{Path, PathBuf},
    process::Command,
};

use anyhow::{Context, Result, anyhow};
use directories::BaseDirs;

const LABEL: &str = "com.local.claude-bridge";

pub fn install_launchd(executable: &Path) -> Result<PathBuf> {
    let base_dirs = BaseDirs::new().context("failed to resolve home directory")?;
    let launch_agents_dir = base_dirs.home_dir().join("Library/LaunchAgents");
    let log_dir = base_dirs.home_dir().join("Library/Logs/claude-bridge");
    fs::create_dir_all(&launch_agents_dir)?;
    fs::create_dir_all(&log_dir)?;

    let plist_path = launch_agents_dir.join(format!("{LABEL}.plist"));
    let psk = env::var("CLAUDE_BRIDGE_PSK")
        .context("CLAUDE_BRIDGE_PSK must be set before install-launchd")?;
    let admin_addr =
        env::var("CLAUDE_BRIDGE_ADMIN_ADDR").unwrap_or_else(|_| "127.0.0.1:37125".into());
    let ws_addr = env::var("CLAUDE_BRIDGE_WS_ADDR").unwrap_or_else(|_| "0.0.0.0:8765".into());
    let state_dir = env::var("CLAUDE_BRIDGE_STATE_DIR").ok();
    let rust_log = env::var("RUST_LOG").unwrap_or_else(|_| "info".into());
    let state_dir_block = if let Some(state_dir) = state_dir.as_deref() {
        format!(
            r#"
    <key>CLAUDE_BRIDGE_STATE_DIR</key>
    <string>{}</string>"#,
            xml_escape(state_dir)
        )
    } else {
        String::new()
    };

    let plist = format!(
        r#"<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key>
  <string>{label}</string>
  <key>ProgramArguments</key>
  <array>
    <string>{exe}</string>
    <string>daemon</string>
  </array>
  <key>RunAtLoad</key>
  <true/>
  <key>KeepAlive</key>
  <true/>
  <key>EnvironmentVariables</key>
  <dict>
    <key>CLAUDE_BRIDGE_PSK</key>
    <string>{psk}</string>
    <key>CLAUDE_BRIDGE_ADMIN_ADDR</key>
    <string>{admin_addr}</string>
    <key>CLAUDE_BRIDGE_WS_ADDR</key>
    <string>{ws_addr}</string>
{state_dir_block}
    <key>RUST_LOG</key>
    <string>{rust_log}</string>
  </dict>
  <key>StandardOutPath</key>
  <string>{stdout_log}</string>
  <key>StandardErrorPath</key>
  <string>{stderr_log}</string>
</dict>
</plist>
"#,
        label = LABEL,
        exe = xml_escape(executable),
        psk = xml_escape(&psk),
        admin_addr = xml_escape(&admin_addr),
        ws_addr = xml_escape(&ws_addr),
        state_dir_block = state_dir_block,
        rust_log = xml_escape(&rust_log),
        stdout_log = xml_escape(log_dir.join("stdout.log")),
        stderr_log = xml_escape(log_dir.join("stderr.log")),
    );

    fs::write(&plist_path, plist)?;

    let uid = current_uid()?;
    let _ = Command::new("launchctl")
        .args([
            "bootout",
            &format!("gui/{uid}"),
            plist_path.to_string_lossy().as_ref(),
        ])
        .status();

    let status = Command::new("launchctl")
        .args([
            "bootstrap",
            &format!("gui/{uid}"),
            plist_path.to_string_lossy().as_ref(),
        ])
        .status()
        .context("failed to run launchctl bootstrap")?;
    if !status.success() {
        return Err(anyhow!("launchctl bootstrap failed"));
    }

    let _ = Command::new("launchctl")
        .args(["kickstart", "-k", &format!("gui/{uid}/{LABEL}")])
        .status();

    Ok(plist_path)
}

pub fn uninstall_launchd() -> Result<Option<PathBuf>> {
    let base_dirs = BaseDirs::new().context("failed to resolve home directory")?;
    let plist_path = base_dirs
        .home_dir()
        .join("Library/LaunchAgents")
        .join(format!("{LABEL}.plist"));

    if !plist_path.exists() {
        return Ok(None);
    }

    let uid = current_uid()?;
    let _ = Command::new("launchctl")
        .args([
            "bootout",
            &format!("gui/{uid}"),
            plist_path.to_string_lossy().as_ref(),
        ])
        .status();

    fs::remove_file(&plist_path)?;
    Ok(Some(plist_path))
}

fn current_uid() -> Result<String> {
    let output = Command::new("id")
        .arg("-u")
        .output()
        .context("failed to execute id -u")?;
    if !output.status.success() {
        return Err(anyhow!("id -u returned non-zero status"));
    }
    Ok(String::from_utf8_lossy(&output.stdout).trim().to_string())
}

fn xml_escape(path: impl AsRef<Path>) -> String {
    path.as_ref()
        .to_string_lossy()
        .replace('&', "&amp;")
        .replace('<', "&lt;")
        .replace('>', "&gt;")
        .replace('"', "&quot;")
        .replace('\'', "&apos;")
}
