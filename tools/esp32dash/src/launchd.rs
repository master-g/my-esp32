use std::{
    fs::{self, File},
    io,
    os::{
        fd::{AsRawFd, BorrowedFd, RawFd},
        unix::fs::MetadataExt,
    },
    path::{Path, PathBuf},
    process::{Command, Output},
};

use anyhow::{Context, Result, anyhow};
use dir_spec::home;
use tracing::warn;

use crate::compat;

const LABEL: &str = "com.local.esp32dash";
const LOG_MAX_BYTES: u64 = 5 * 1024 * 1024;

fn log_dir(home_dir: &Path) -> PathBuf {
    home_dir.join("Library/Logs/esp32dash")
}

pub fn install_launchd(executable: &Path) -> Result<PathBuf> {
    let uid = current_uid()?;
    validate_launch_agent_uid(&uid)?;
    let home_dir = home().ok_or_else(|| anyhow!("failed to resolve home directory"))?;
    let launch_agents_dir = home_dir.join("Library/LaunchAgents");
    let log_dir = log_dir(&home_dir);
    fs::create_dir_all(&launch_agents_dir)
        .with_context(|| format!("failed to create {}", launch_agents_dir.display()))?;
    fs::create_dir_all(&log_dir)
        .with_context(|| format!("failed to create {}", log_dir.display()))?;

    let plist_path = launch_agents_dir.join(format!("{LABEL}.plist"));
    let serial_port = compat::serial_port();
    let state_dir = compat::state_dir();
    let config_path = compat::env_value("ESP32DASH_CONFIG");
    let rust_log = std::env::var("RUST_LOG").unwrap_or_else(|_| "info".into());

    let admin_addr_block = optional_env_var_block(
        "ESP32DASH_ADMIN_ADDR",
        compat::env_value("ESP32DASH_ADMIN_ADDR").as_deref(),
    );
    let serial_port_block = optional_env_var_block("ESP32DASH_SERIAL_PORT", serial_port.as_deref());
    let serial_baud_block = optional_env_var_block(
        "ESP32DASH_SERIAL_BAUD",
        compat::env_value("ESP32DASH_SERIAL_BAUD").as_deref(),
    );
    let state_dir_block = optional_env_var_block("ESP32DASH_STATE_DIR", state_dir.as_deref());
    let config_path_block = optional_env_var_block("ESP32DASH_CONFIG", config_path.as_deref());

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
    <string>agent</string>
    <string>run</string>
  </array>
  <key>RunAtLoad</key>
  <true/>
  <key>KeepAlive</key>
  <true/>
  <key>EnvironmentVariables</key>
  <dict>
{admin_addr_block}
{serial_port_block}
{serial_baud_block}
{state_dir_block}
{config_path_block}
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
        admin_addr_block = admin_addr_block,
        serial_port_block = serial_port_block,
        serial_baud_block = serial_baud_block,
        state_dir_block = state_dir_block,
        config_path_block = config_path_block,
        rust_log = xml_escape(&rust_log),
        stdout_log = xml_escape(log_dir.join("stdout.log")),
        stderr_log = xml_escape(log_dir.join("stderr.log")),
    );

    fs::write(&plist_path, plist).with_context(|| {
        format!(
            "failed to write {}; if a previous sudo run created a root-owned plist, remove or chown it and rerun without sudo",
            plist_path.display()
        )
    })?;

    bootout_path_if_present(&uid, &plist_path);

    let output = Command::new("launchctl")
        .args(["bootstrap", &format!("gui/{uid}"), plist_path.to_string_lossy().as_ref()])
        .output()
        .context("failed to run launchctl bootstrap")?;
    if !output.status.success() {
        return Err(anyhow!("launchctl bootstrap failed: {}", command_failure_detail(&output)));
    }

    let _ =
        Command::new("launchctl").args(["kickstart", "-k", &format!("gui/{uid}/{LABEL}")]).status();

    Ok(plist_path)
}

pub fn uninstall_launchd() -> Result<Option<PathBuf>> {
    let uid = current_uid()?;
    validate_launch_agent_uid(&uid)?;
    let home_dir = home().ok_or_else(|| anyhow!("failed to resolve home directory"))?;
    let plist_path = home_dir.join("Library/LaunchAgents").join(format!("{LABEL}.plist"));

    bootout_service_if_present(&uid, LABEL);

    if !plist_path.exists() {
        return Ok(None);
    }

    fs::remove_file(&plist_path)?;
    Ok(Some(plist_path))
}

/// launchd writes stdout/stderr straight into the log files and never rotates them. When a
/// file this process writes to grows past LOG_MAX_BYTES, keep one `.1` copy and truncate it
/// in place (launchd holds the fd, so rename-based rotation would keep writing the old inode).
/// Caps each log at ~2x LOG_MAX_BYTES on disk. No-op when stdout/stderr point elsewhere.
pub fn rotate_service_logs() {
    let Some(dir) = home().map(|home_dir| log_dir(&home_dir)) else {
        return;
    };
    for (fd, name) in [(libc::STDOUT_FILENO, "stdout.log"), (libc::STDERR_FILENO, "stderr.log")] {
        if let Err(err) = rotate_fd_log(fd, &dir.join(name), LOG_MAX_BYTES) {
            warn!("failed to rotate {name}: {err}");
        }
    }
}

fn rotate_fd_log(fd: RawFd, path: &Path, max_bytes: u64) -> io::Result<bool> {
    // SAFETY: fd is a descriptor this process owns for its whole lifetime (stdout/stderr, or
    // an open File in tests); the borrow is immediately duplicated.
    let file = File::from(unsafe { BorrowedFd::borrow_raw(fd) }.try_clone_to_owned()?);
    let fd_meta = file.metadata()?;
    let path_meta = match fs::metadata(path) {
        Ok(meta) => meta,
        Err(err) if err.kind() == io::ErrorKind::NotFound => return Ok(false),
        Err(err) => return Err(err),
    };
    if !fd_meta.is_file() || fd_meta.dev() != path_meta.dev() || fd_meta.ino() != path_meta.ino() {
        return Ok(false);
    }

    // Without O_APPEND, writes after truncation would continue at the old offset and leave a
    // sparse hole of the same size. The flag lives on the shared file description.
    // SAFETY: plain fcntl flag query/update on a valid descriptor.
    let flags = unsafe { libc::fcntl(file.as_raw_fd(), libc::F_GETFL) };
    if flags < 0 {
        return Err(io::Error::last_os_error());
    }
    if flags & libc::O_APPEND == 0
        && unsafe { libc::fcntl(file.as_raw_fd(), libc::F_SETFL, flags | libc::O_APPEND) } < 0
    {
        return Err(io::Error::last_os_error());
    }

    if fd_meta.len() <= max_bytes {
        return Ok(false);
    }
    let mut backup = path.as_os_str().to_owned();
    backup.push(".1");
    // ponytail: lines written between copy and truncate are lost; fine at this log volume.
    fs::copy(path, &backup)?;
    file.set_len(0)?;
    Ok(true)
}

fn bootout_service_if_present(uid: &str, label: &str) {
    let _ = Command::new("launchctl").args(["bootout", &format!("gui/{uid}/{label}")]).status();
}

fn bootout_path_if_present(uid: &str, plist_path: &Path) {
    let _ = Command::new("launchctl")
        .args(["bootout", &format!("gui/{uid}"), plist_path.to_string_lossy().as_ref()])
        .status();
}

fn current_uid() -> Result<String> {
    let output = Command::new("id").arg("-u").output().context("failed to execute id -u")?;
    if !output.status.success() {
        return Err(anyhow!("id -u returned non-zero status"));
    }
    Ok(String::from_utf8_lossy(&output.stdout).trim().to_string())
}

fn validate_launch_agent_uid(uid: &str) -> Result<()> {
    if uid == "0" {
        return Err(anyhow!(
            "launchd install manages ~/Library/LaunchAgents for the logged-in macOS user and cannot run as root; rerun without sudo"
        ));
    }
    Ok(())
}

fn command_failure_detail(output: &Output) -> String {
    let stderr = String::from_utf8_lossy(&output.stderr).trim().to_string();
    if !stderr.is_empty() {
        return stderr;
    }

    let stdout = String::from_utf8_lossy(&output.stdout).trim().to_string();
    if !stdout.is_empty() {
        return stdout;
    }

    match output.status.code() {
        Some(code) => format!("exit code {code}"),
        None => "terminated by signal".into(),
    }
}

fn optional_env_var_block(key: &str, value: Option<&str>) -> String {
    value.map_or_else(String::new, |value| {
        format!(
            r#"    <key>{key}</key>
    <string>{value}</string>
"#,
            key = key,
            value = xml_escape(value),
        )
    })
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

#[cfg(test)]
mod tests {
    use std::{
        fs::{self, OpenOptions},
        io::Write,
        os::fd::AsRawFd,
    };

    use super::{optional_env_var_block, rotate_fd_log, validate_launch_agent_uid};

    #[test]
    fn rotate_fd_log_keeps_backup_and_truncates_without_hole() {
        let dir = std::env::temp_dir().join(format!("esp32dash-logrot-{}", std::process::id()));
        fs::create_dir_all(&dir).unwrap();
        let path = dir.join("stdout.log");
        let other = dir.join("other.log");
        fs::write(&other, "x").unwrap();
        // Plain write mode, like a descriptor opened without O_APPEND.
        let mut file =
            OpenOptions::new().create(true).truncate(true).write(true).open(&path).unwrap();
        file.write_all(&[b'a'; 64]).unwrap();

        assert!(!rotate_fd_log(file.as_raw_fd(), &path, 100).unwrap(), "under limit");
        assert!(!rotate_fd_log(file.as_raw_fd(), &other, 1).unwrap(), "fd is not that path");

        file.write_all(&[b'b'; 64]).unwrap();
        assert!(rotate_fd_log(file.as_raw_fd(), &path, 100).unwrap());
        assert_eq!(fs::metadata(dir.join("stdout.log.1")).unwrap().len(), 128);
        assert_eq!(fs::metadata(&path).unwrap().len(), 0);

        file.write_all(b"next\n").unwrap();
        assert_eq!(fs::read(&path).unwrap(), b"next\n");

        let _ = fs::remove_dir_all(&dir);
    }

    #[test]
    fn launch_agent_rejects_root_uid() {
        let err = validate_launch_agent_uid("0").expect_err("root uid should be rejected");
        assert!(err.to_string().contains("without sudo"));
    }

    #[test]
    fn launch_agent_accepts_non_root_uid() {
        validate_launch_agent_uid("501").expect("non-root uid should be accepted");
    }

    #[test]
    fn optional_env_var_block_omits_missing_values() {
        assert_eq!(optional_env_var_block("ESP32DASH_CONFIG", None), "");
    }

    #[test]
    fn optional_env_var_block_renders_present_values() {
        let block = optional_env_var_block("ESP32DASH_CONFIG", Some("/tmp/esp32dash/config.toml"));
        assert!(block.contains("ESP32DASH_CONFIG"));
        assert!(block.contains("/tmp/esp32dash/config.toml"));
    }
}
