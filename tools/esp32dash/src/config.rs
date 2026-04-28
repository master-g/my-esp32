use std::{
    env, fmt,
    fs::{self, File},
    io::{ErrorKind, Read, Write},
    path::{Path, PathBuf},
};

use anyhow::{Context, Result, anyhow};
use dir_spec::{data_home, home, xdg_config_home_or};
use serde::Deserialize;
use tracing::warn;

use crate::compat;

#[cfg(unix)]
use std::os::unix::fs::{OpenOptionsExt, PermissionsExt};

pub const DEFAULT_EMOTION_TIMEOUT_SECS: u64 = 3;
pub const DEFAULT_EMOTION_MAX_TOKENS: u32 = 256;

pub const DEFAULT_APPROVAL_TIMEOUT_SECS: u64 = 600;
pub const DEFAULT_ELICITATION_TIMEOUT_SECS: u64 = 120;
pub const DEFAULT_QUESTION_TIMEOUT_SECS: u64 = 180;

const APP_DIR_NAME: &str = "esp32dash";
const CONFIG_FILE_NAME: &str = "config.toml";

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AppConfig {
    pub admin_addr: Option<String>,
    pub state_dir: Option<PathBuf>,
    pub serial_port: Option<String>,
    pub serial_baud: Option<u32>,
    pub emotion: EmotionConfig,
    pub approval: ApprovalConfig,
}

impl Default for AppConfig {
    fn default() -> Self {
        Self {
            admin_addr: None,
            state_dir: None,
            serial_port: None,
            serial_baud: None,
            emotion: EmotionConfig::default(),
            approval: ApprovalConfig::default(),
        }
    }
}

#[derive(Clone, PartialEq, Eq)]
pub struct EmotionConfig {
    pub enabled: bool,
    pub api_base_url: Option<String>,
    pub model: Option<String>,
    pub api_key: Option<String>,
    pub timeout_secs: u64,
    pub max_tokens: u32,
}

impl fmt::Debug for EmotionConfig {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("EmotionConfig")
            .field("enabled", &self.enabled)
            .field("api_base_url", &self.api_base_url)
            .field("model", &self.model)
            .field("api_key", &self.api_key.as_ref().map(|_| "<redacted>"))
            .field("timeout_secs", &self.timeout_secs)
            .field("max_tokens", &self.max_tokens)
            .finish()
    }
}

impl Default for EmotionConfig {
    fn default() -> Self {
        Self {
            enabled: false,
            api_base_url: None,
            model: None,
            api_key: None,
            timeout_secs: DEFAULT_EMOTION_TIMEOUT_SECS,
            max_tokens: DEFAULT_EMOTION_MAX_TOKENS,
        }
    }
}

impl EmotionConfig {
    pub fn has_provider_config(&self) -> bool {
        self.api_base_url.is_some() && self.model.is_some() && self.api_key.is_some()
    }

    fn normalize(&mut self) {
        if !self.has_provider_config() {
            self.enabled = false;
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ApprovalConfig {
    pub timeout_secs: u64,
    pub elicitation_timeout_secs: u64,
    pub question_timeout_secs: u64,
}

impl Default for ApprovalConfig {
    fn default() -> Self {
        Self {
            timeout_secs: DEFAULT_APPROVAL_TIMEOUT_SECS,
            elicitation_timeout_secs: DEFAULT_ELICITATION_TIMEOUT_SECS,
            question_timeout_secs: DEFAULT_QUESTION_TIMEOUT_SECS,
        }
    }
}

#[derive(Debug, Default, Deserialize)]
struct FileAppConfig {
    #[serde(default)]
    admin_addr: Option<String>,
    #[serde(default)]
    state_dir: Option<PathBuf>,
    #[serde(default)]
    serial_port: Option<String>,
    #[serde(default)]
    serial_baud: Option<u32>,
    #[serde(default)]
    emotion: FileEmotionConfig,
    #[serde(default)]
    approval: FileApprovalConfig,
}

#[derive(Debug, Default, Deserialize)]
struct FileEmotionConfig {
    #[serde(default)]
    enabled: Option<bool>,
    #[serde(default)]
    api_base_url: Option<String>,
    #[serde(default)]
    model: Option<String>,
    #[serde(default)]
    api_key: Option<String>,
    #[serde(default)]
    timeout_secs: Option<u64>,
    #[serde(default)]
    max_tokens: Option<u32>,
}

#[derive(Debug, Default, Deserialize)]
struct FileApprovalConfig {
    #[serde(default)]
    timeout_secs: Option<u64>,
    #[serde(default)]
    elicitation_timeout_secs: Option<u64>,
    #[serde(default)]
    question_timeout_secs: Option<u64>,
}

impl AppConfig {
    #[allow(dead_code)]
    pub fn load() -> Result<Self> {
        load_with_default_bootstrap(false)
    }

    pub fn load_for_agent_run() -> Result<Self> {
        load_with_default_bootstrap(true)
    }
}

fn load_with_default_bootstrap(bootstrap_default: bool) -> Result<AppConfig> {
    let explicit_config_path = env::var("ESP32DASH_CONFIG")
        .ok()
        .and_then(|value| trimmed_non_empty(&value))
        .map(PathBuf::from);
    let default_path = default_config_path()?;
    let config_path =
        resolve_config_path(explicit_config_path.as_deref(), &default_path, bootstrap_default)?;
    load_from_path(config_path.as_deref())
}

fn resolve_config_path(
    explicit_config_path: Option<&Path>,
    default_path: &Path,
    bootstrap_default: bool,
) -> Result<Option<PathBuf>> {
    if let Some(path) = explicit_config_path {
        return Ok(Some(path.to_path_buf()));
    }

    if default_path.exists() {
        if default_path.is_dir() {
            return Err(anyhow!(
                "config path {} points to a directory, expected a file",
                default_path.display()
            ));
        }
        return Ok(Some(default_path.to_path_buf()));
    }

    if bootstrap_default {
        bootstrap_default_config(default_path)?;
        return Ok(Some(default_path.to_path_buf()));
    }

    Ok(None)
}

pub fn default_state_dir() -> Result<PathBuf> {
    if let Some(path) = data_home() {
        return Ok(path.join(APP_DIR_NAME));
    }

    home()
        .map(|path| path.join(".local").join("share").join(APP_DIR_NAME))
        .ok_or_else(|| anyhow!("failed to resolve default state directory"))
}

fn default_config_path() -> Result<PathBuf> {
    Ok(default_config_dir()?.join(CONFIG_FILE_NAME))
}

fn default_config_dir() -> Result<PathBuf> {
    Ok(default_config_home()?.join(APP_DIR_NAME))
}

fn default_config_home() -> Result<PathBuf> {
    if let Some(home_dir) = home() {
        return Ok(xdg_config_home_or(home_dir.join(".config")));
    }

    Err(anyhow!("failed to resolve default config home"))
}

fn bootstrap_default_config(config_path: &Path) -> Result<()> {
    let config_dir = config_path
        .parent()
        .ok_or_else(|| anyhow!("config path {} has no parent directory", config_path.display()))?;

    fs::create_dir_all(config_dir)
        .with_context(|| format!("failed to create config directory {}", config_dir.display()))?;
    set_secure_dir_permissions(config_dir)?;

    if config_path.exists() {
        if config_path.is_dir() {
            return Err(anyhow!(
                "config path {} points to a directory, expected a file",
                config_path.display()
            ));
        }
        return Ok(());
    }

    let mut file = match open_bootstrap_config(config_path) {
        Ok(file) => file,
        Err(err) if err.kind() == ErrorKind::AlreadyExists => return Ok(()),
        Err(err) => {
            return Err(err).with_context(|| {
                format!("failed to create default config {}", config_path.display())
            });
        }
    };
    file.write_all(default_config_template().as_bytes())
        .with_context(|| format!("failed to write default config {}", config_path.display()))?;
    Ok(())
}

#[cfg(unix)]
fn open_bootstrap_config(config_path: &Path) -> std::io::Result<File> {
    let mut options = fs::OpenOptions::new();
    options.write(true).create_new(true).mode(0o600);
    options.open(config_path)
}

#[cfg(not(unix))]
fn open_bootstrap_config(config_path: &Path) -> std::io::Result<File> {
    fs::OpenOptions::new().write(true).create_new(true).open(config_path)
}

fn default_config_template() -> String {
    format!(
        "# Auto-generated by esp32dash on first launch.\n\
         # Uncomment and adjust any values you want to override.\n\
         # admin_addr = \"{}\"\n\
         # state_dir = \"/absolute/path/to/esp32dash-state\"\n\
         # serial_port = \"/dev/cu.usbmodem1234\"\n\
         # serial_baud = {}\n\
         \n\
         [emotion]\n\
         enabled = false\n\
         # api_base_url = \"https://api.anthropic.com/v1/messages\"\n\
         # model = \"claude-haiku-4-5-20251001\"\n\
         # api_key = \"replace-me\"\n\
         timeout_secs = {}\n\
         max_tokens = {}\n\
         \n\
         [approval]\n\
         # timeout_secs = {}          # PermissionRequest timeout (seconds)\n\
         # elicitation_timeout_secs = {}  # Elicitation timeout (seconds)\n\
         # question_timeout_secs = {}     # AskUserQuestion timeout (seconds)\n",
        compat::DEFAULT_ADMIN_ADDR,
        compat::DEFAULT_SERIAL_BAUD,
        DEFAULT_EMOTION_TIMEOUT_SECS,
        DEFAULT_EMOTION_MAX_TOKENS,
        DEFAULT_APPROVAL_TIMEOUT_SECS,
        DEFAULT_ELICITATION_TIMEOUT_SECS,
        DEFAULT_QUESTION_TIMEOUT_SECS,
    )
}

fn load_from_path(config_path: Option<&Path>) -> Result<AppConfig> {
    let mut resolved = AppConfig::default();

    if let Some(path) = config_path {
        let file_config = load_file_config(path)?;
        merge_file_config(&mut resolved, file_config);
    }

    resolved.emotion.normalize();
    Ok(resolved)
}

fn load_file_config(path: &Path) -> Result<FileAppConfig> {
    let mut file = File::open(path)
        .with_context(|| format!("failed to read esp32dash config {}", path.display()))?;
    let secure_permissions = has_secure_permissions(&file)?;
    let mut raw = String::new();
    file.read_to_string(&mut raw)
        .with_context(|| format!("failed to read esp32dash config {}", path.display()))?;
    let mut config: FileAppConfig = toml::from_str(&raw)
        .with_context(|| format!("failed to parse esp32dash config {}", path.display()))?;

    if file_config_has_secret(&config) && !secure_permissions {
        warn!(
            "ignoring emotion.api_key from {} because the file is group/world-readable",
            path.display()
        );
        config.emotion.api_key = None;
    }

    Ok(config)
}

fn merge_file_config(resolved: &mut AppConfig, file: FileAppConfig) {
    if let Some(admin_addr) = file.admin_addr.and_then(|value| trimmed_non_empty(&value)) {
        resolved.admin_addr = Some(admin_addr);
    }
    if let Some(state_dir) = file.state_dir.filter(|value| !value.as_os_str().is_empty()) {
        resolved.state_dir = Some(state_dir);
    }
    if let Some(serial_port) = file.serial_port.and_then(|value| trimmed_non_empty(&value)) {
        resolved.serial_port = Some(serial_port);
    }
    if let Some(serial_baud) = file.serial_baud {
        resolved.serial_baud = Some(serial_baud);
    }

    if let Some(enabled) = file.emotion.enabled {
        resolved.emotion.enabled = enabled;
    }
    if let Some(api_base_url) =
        file.emotion.api_base_url.and_then(|value| trimmed_non_empty(&value))
    {
        resolved.emotion.api_base_url = Some(api_base_url);
    }
    if let Some(model) = file.emotion.model.and_then(|value| trimmed_non_empty(&value)) {
        resolved.emotion.model = Some(model);
    }
    if let Some(api_key) = file.emotion.api_key.and_then(|value| trimmed_non_empty(&value)) {
        resolved.emotion.api_key = Some(api_key);
    }
    if let Some(timeout_secs) = file.emotion.timeout_secs {
        resolved.emotion.timeout_secs = timeout_secs;
    }
    if let Some(max_tokens) = file.emotion.max_tokens {
        resolved.emotion.max_tokens = max_tokens;
    }

    if let Some(timeout_secs) = file.approval.timeout_secs {
        resolved.approval.timeout_secs = timeout_secs;
    }
    if let Some(elicitation_timeout_secs) = file.approval.elicitation_timeout_secs {
        resolved.approval.elicitation_timeout_secs = elicitation_timeout_secs;
    }
    if let Some(question_timeout_secs) = file.approval.question_timeout_secs {
        resolved.approval.question_timeout_secs = question_timeout_secs;
    }
}

fn file_config_has_secret(file: &FileAppConfig) -> bool {
    file.emotion.api_key.as_deref().and_then(trimmed_non_empty).is_some()
}

#[cfg(unix)]
fn has_secure_permissions(file: &File) -> Result<bool> {
    let mode =
        file.metadata().context("failed to inspect config file permissions")?.permissions().mode();
    Ok(mode & 0o077 == 0)
}

#[cfg(not(unix))]
fn has_secure_permissions(_file: &File) -> Result<bool> {
    Ok(true)
}

#[cfg(unix)]
fn set_secure_dir_permissions(path: &Path) -> Result<()> {
    fs::set_permissions(path, fs::Permissions::from_mode(0o700))
        .with_context(|| format!("failed to secure {}", path.display()))
}

#[cfg(not(unix))]
fn set_secure_dir_permissions(_path: &Path) -> Result<()> {
    Ok(())
}

fn trimmed_non_empty(value: &str) -> Option<String> {
    let trimmed = value.trim();
    (!trimmed.is_empty()).then(|| trimmed.to_string())
}

#[cfg(test)]
mod tests {
    use std::{
        fs,
        path::{Path, PathBuf},
        time::SystemTime,
    };

    use anyhow::Result;

    use super::*;

    #[cfg(unix)]
    fn set_mode(path: &Path, mode: u32) -> Result<()> {
        fs::set_permissions(path, fs::Permissions::from_mode(mode))?;
        Ok(())
    }

    fn temp_dir(label: &str) -> PathBuf {
        let unique = SystemTime::now()
            .duration_since(SystemTime::UNIX_EPOCH)
            .expect("clock should be after epoch")
            .as_nanos();
        std::env::temp_dir().join(format!("esp32dash-config-test-{label}-{unique}"))
    }

    fn write_file(path: &Path, contents: &str) -> Result<()> {
        if let Some(parent) = path.parent() {
            fs::create_dir_all(parent)?;
        }
        fs::write(path, contents)?;
        Ok(())
    }

    #[test]
    fn defaults_without_config_file() -> Result<()> {
        let config = load_from_path(None)?;
        assert_eq!(config, AppConfig::default());
        Ok(())
    }

    #[test]
    fn resolve_config_path_without_bootstrap_skips_missing_default() -> Result<()> {
        let root = temp_dir("resolve-missing");
        let config_path = root.join(".config").join(APP_DIR_NAME).join(CONFIG_FILE_NAME);

        let resolved = resolve_config_path(None, &config_path, false)?;

        assert_eq!(resolved, None);
        assert!(!config_path.exists());

        let _ = fs::remove_dir_all(root);
        Ok(())
    }

    #[test]
    fn bootstrap_writes_default_config_file() -> Result<()> {
        let root = temp_dir("bootstrap");
        let config_path = root.join(".config").join(APP_DIR_NAME).join(CONFIG_FILE_NAME);

        bootstrap_default_config(&config_path)?;

        assert!(config_path.is_file());
        let written = fs::read_to_string(&config_path)?;
        assert!(written.contains("[emotion]"));
        assert!(written.contains("enabled = false"));
        assert!(written.contains("api_base_url"));

        let _ = fs::remove_dir_all(root);
        Ok(())
    }

    #[cfg(unix)]
    #[test]
    fn bootstrap_creates_secure_config_file() -> Result<()> {
        let root = temp_dir("bootstrap-mode");
        let config_path = root.join(".config").join(APP_DIR_NAME).join(CONFIG_FILE_NAME);

        bootstrap_default_config(&config_path)?;

        let mode = fs::metadata(&config_path)?.permissions().mode();
        assert_eq!(mode & 0o777, 0o600);

        let _ = fs::remove_dir_all(root);
        Ok(())
    }

    #[test]
    fn local_config_overrides_base_fields() -> Result<()> {
        let root = temp_dir("config");
        let config_path = root.join("config.toml");
        write_file(
            &config_path,
            r#"
admin_addr = "127.0.0.1:4000"
state_dir = "/tmp/esp32dash-state"
serial_port = "/dev/cu.usbmodem-test"
serial_baud = 230400

[emotion]
enabled = true
api_base_url = "https://config.example.com/v1/messages"
model = "claude-test-model"
api_key = "token-123"
timeout_secs = 5
max_tokens = 64
"#,
        )?;
        #[cfg(unix)]
        set_mode(&config_path, 0o600)?;

        let config = load_from_path(Some(&config_path))?;

        assert_eq!(config.admin_addr.as_deref(), Some("127.0.0.1:4000"));
        assert_eq!(config.state_dir.as_deref(), Some(Path::new("/tmp/esp32dash-state")));
        assert_eq!(config.serial_port.as_deref(), Some("/dev/cu.usbmodem-test"));
        assert_eq!(config.serial_baud, Some(230_400));
        assert!(config.emotion.enabled);
        assert_eq!(
            config.emotion.api_base_url.as_deref(),
            Some("https://config.example.com/v1/messages")
        );
        assert_eq!(config.emotion.model.as_deref(), Some("claude-test-model"));
        assert_eq!(config.emotion.api_key.as_deref(), Some("token-123"));
        assert_eq!(config.emotion.timeout_secs, 5);
        assert_eq!(config.emotion.max_tokens, 64);

        let _ = fs::remove_dir_all(root);
        Ok(())
    }

    #[test]
    fn incomplete_emotion_provider_stays_disabled() -> Result<()> {
        let root = temp_dir("disabled");
        let config_path = root.join("config.toml");
        write_file(
            &config_path,
            r#"
[emotion]
enabled = true
api_base_url = "https://config.example.com/v1/messages"
model = "claude-test-model"
"#,
        )?;

        let config = load_from_path(Some(&config_path))?;

        assert!(!config.emotion.enabled);
        assert_eq!(
            config.emotion.api_base_url.as_deref(),
            Some("https://config.example.com/v1/messages")
        );
        assert_eq!(config.emotion.model.as_deref(), Some("claude-test-model"));
        assert_eq!(config.emotion.api_key, None);

        let _ = fs::remove_dir_all(root);
        Ok(())
    }

    #[cfg(unix)]
    #[test]
    fn insecure_config_file_drops_api_key_and_disables_emotion() -> Result<()> {
        let root = temp_dir("config-perms");
        let config_path = root.join("config.toml");
        write_file(
            &config_path,
            r#"
serial_baud = 230400

[emotion]
enabled = true
api_base_url = "https://config.example.com/v1/messages"
model = "claude-test-model"
api_key = "secret-key"
"#,
        )?;
        set_mode(&config_path, 0o644)?;

        let config = load_from_path(Some(&config_path))?;

        assert_eq!(config.serial_baud, Some(230_400));
        assert!(!config.emotion.enabled);
        assert_eq!(config.emotion.api_key, None);

        let _ = fs::remove_dir_all(root);
        Ok(())
    }

    #[test]
    fn default_state_dir_uses_expected_suffix() -> Result<()> {
        let state_dir = default_state_dir()?;
        assert!(state_dir.ends_with(APP_DIR_NAME));
        Ok(())
    }
}
