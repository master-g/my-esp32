use std::{
    fs,
    path::{Path, PathBuf},
};

use anyhow::{Context, Result, anyhow};
use dir_spec::home;
use inquire::{Confirm, InquireError};
use serde::Serialize;
use serde_json::{Map, Value, json};

const CLAUDE_DIR_NAME: &str = ".claude";
const HOOK_SCRIPT_NAME: &str = "esp32dash-hook.sh";
const SETTINGS_NAME: &str = "settings.json";
const HOOK_COMMAND_PATH: &str = "~/.claude/hooks/esp32dash-hook.sh";

#[derive(Debug, Clone, Copy)]
struct HookSpec {
    event: &'static str,
    matcher: Option<&'static str>,
}

const HOOK_SPECS: &[HookSpec] = &[
    HookSpec {
        event: "SessionStart",
        matcher: None,
    },
    HookSpec {
        event: "SessionEnd",
        matcher: None,
    },
    HookSpec {
        event: "Notification",
        matcher: None,
    },
    HookSpec {
        event: "UserPromptSubmit",
        matcher: None,
    },
    HookSpec {
        event: "PreToolUse",
        matcher: Some("*"),
    },
    HookSpec {
        event: "PostToolUse",
        matcher: Some("*"),
    },
    HookSpec {
        event: "PostToolUseFailure",
        matcher: Some("*"),
    },
    HookSpec {
        event: "PermissionDenied",
        matcher: Some("*"),
    },
    HookSpec {
        event: "Elicitation",
        matcher: Some("*"),
    },
    HookSpec {
        event: "ElicitationResult",
        matcher: Some("*"),
    },
    HookSpec {
        event: "Stop",
        matcher: None,
    },
    HookSpec {
        event: "StopFailure",
        matcher: None,
    },
    HookSpec {
        event: "SubagentStart",
        matcher: None,
    },
    HookSpec {
        event: "SubagentStop",
        matcher: None,
    },
    HookSpec {
        event: "PreCompact",
        matcher: Some("auto"),
    },
    HookSpec {
        event: "PreCompact",
        matcher: Some("manual"),
    },
    HookSpec {
        event: "PostCompact",
        matcher: Some("auto"),
    },
    HookSpec {
        event: "PostCompact",
        matcher: Some("manual"),
    },
    HookSpec {
        event: "PermissionRequest",
        matcher: Some("*"),
    },
];

#[derive(Debug, Clone, Serialize)]
pub struct InstallHooksResult {
    pub ok: bool,
    pub hook_script_path: String,
    pub settings_path: String,
    pub script_written: bool,
    pub settings_updated: bool,
    pub changes_applied: bool,
}

#[derive(Debug)]
struct InstallAnalysis {
    hook_script_path: PathBuf,
    settings_path: PathBuf,
    hook_script_contents: String,
    script_written: bool,
    settings_updated: bool,
    updated_specs: Vec<String>,
    settings_json: Value,
}

#[derive(Debug, Clone, Serialize)]
pub struct ClaudeUninstallResult {
    pub settings_path: String,
    pub settings_updated: bool,
    pub script_removed: bool,
}

fn home_claude_dir() -> Result<PathBuf> {
    Ok(home().ok_or_else(|| anyhow!("failed to resolve home directory"))?.join(CLAUDE_DIR_NAME))
}

pub fn install_hooks(executable: &Path, force: bool) -> Result<InstallHooksResult> {
    let claude_dir = home_claude_dir()?;
    let analysis = analyze_install(&claude_dir, executable)?;
    let changes = analysis.script_written || analysis.settings_updated;

    if changes && !force {
        let confirmed =
            match Confirm::new(&build_confirm_message(&analysis)).with_default(true).prompt() {
                Ok(value) => value,
                Err(InquireError::OperationCanceled) | Err(InquireError::OperationInterrupted) => {
                    return Err(anyhow!("installation cancelled by user"));
                }
                Err(err) => return Err(err).context("failed to prompt for hook installation"),
            };

        if !confirmed {
            return Err(anyhow!("installation cancelled by user"));
        }
    }

    if changes {
        apply_install(&claude_dir, &analysis)?;
    }

    Ok(InstallHooksResult {
        ok: true,
        hook_script_path: analysis.hook_script_path.to_string_lossy().into_owned(),
        settings_path: analysis.settings_path.to_string_lossy().into_owned(),
        script_written: analysis.script_written,
        settings_updated: analysis.settings_updated,
        changes_applied: changes,
    })
}

pub fn uninstall_hooks() -> Result<ClaudeUninstallResult> {
    uninstall_claude_hooks(&home_claude_dir()?)
}

fn analyze_install(claude_dir: &Path, executable: &Path) -> Result<InstallAnalysis> {
    let hook_script_path = claude_dir.join("hooks").join(HOOK_SCRIPT_NAME);
    let settings_path = claude_dir.join(SETTINGS_NAME);
    let hook_script_contents = render_hook_script(executable);
    let existing_script = fs::read_to_string(&hook_script_path).ok();
    let script_written = existing_script.as_deref() != Some(hook_script_contents.as_str());
    let command_variants = hook_command_variants(&hook_script_path);

    let mut settings_json = if settings_path.exists() {
        let raw = fs::read_to_string(&settings_path)
            .with_context(|| format!("failed to read {}", settings_path.display()))?;
        serde_json::from_str::<Value>(&raw)
            .with_context(|| format!("failed to parse {}", settings_path.display()))?
    } else {
        Value::Object(Map::new())
    };

    let updated_specs =
        ensure_settings_hooks(&mut settings_json, HOOK_COMMAND_PATH, &command_variants)?;
    let settings_updated = !updated_specs.is_empty();

    Ok(InstallAnalysis {
        hook_script_path,
        settings_path,
        hook_script_contents,
        script_written,
        settings_updated,
        updated_specs,
        settings_json,
    })
}

/// Write `value` as pretty-printed JSON to `path` atomically via a sibling
/// temp file + rename, so a crash never truncates the user's settings.json.
fn atomic_write_json(path: &Path, value: &Value) -> Result<()> {
    let formatted = serde_json::to_string_pretty(value)
        .with_context(|| format!("failed to serialize {}", path.display()))?;
    let file_name = path.file_name().and_then(|n| n.to_str()).unwrap_or("settings.json");
    let tmp_path = path.with_file_name(format!("{file_name}.tmp"));
    fs::write(&tmp_path, format!("{formatted}\n"))
        .with_context(|| format!("failed to write {}", tmp_path.display()))?;
    fs::rename(&tmp_path, path).with_context(|| {
        format!("failed to rename {} to {}", tmp_path.display(), path.display())
    })?;
    Ok(())
}

fn apply_install(claude_dir: &Path, analysis: &InstallAnalysis) -> Result<()> {
    let hooks_dir = claude_dir.join("hooks");
    fs::create_dir_all(&hooks_dir)
        .with_context(|| format!("failed to create {}", hooks_dir.display()))?;

    if analysis.script_written {
        fs::write(&analysis.hook_script_path, &analysis.hook_script_contents).with_context(
            || format!("failed to write hook script {}", analysis.hook_script_path.display()),
        )?;
    }
    ensure_executable(&analysis.hook_script_path)?;

    if analysis.settings_updated {
        atomic_write_json(&analysis.settings_path, &analysis.settings_json)?;
    }

    Ok(())
}

fn build_confirm_message(analysis: &InstallAnalysis) -> String {
    let mut lines = vec!["Install esp32dash hooks?".to_string()];
    if analysis.script_written {
        lines.push(format!("Write hook script: {}", analysis.hook_script_path.display()));
    }
    if analysis.settings_updated {
        lines.push(format!("Update settings: {}", analysis.settings_path.display()));
        lines.push(format!("Add hook entries: {}", analysis.updated_specs.join(", ")));
    }
    lines.join("\n")
}

fn render_hook_script(executable: &Path) -> String {
    let default_bin = shell_bin_path(executable, home().as_deref());
    format!(
        r#"#!/bin/sh

DEFAULT_BIN={default_bin}
BIN="${{ESP32DASH_BIN:-$DEFAULT_BIN}}"
export ESP32DASH_CLAUDE_PID="${{PPID:-}}"

if [ ! -x "$BIN" ]; then
  if command -v esp32dash >/dev/null 2>&1; then
    BIN=esp32dash
  else
    exit 0
  fi
fi

# Every event takes the same path. The device is a read-only notifier, so the
# hook never prints a decision and Claude Code's own prompt stays in charge.
"$BIN" claude ingest --event-from-stdin >/dev/null 2>&1
exit 0
"#
    )
}

/// Paths under $HOME render as `"${HOME}/..."` so one script works for every user/OS the
/// dotfiles sync it to; anything else is single-quoted verbatim.
fn shell_bin_path(executable: &Path, home_dir: Option<&Path>) -> String {
    match home_dir.and_then(|home_dir| executable.strip_prefix(home_dir).ok()) {
        Some(rest) => {
            let rest = rest.to_string_lossy();
            let mut escaped = String::with_capacity(rest.len());
            for ch in rest.chars() {
                if matches!(ch, '\\' | '"' | '$' | '`') {
                    escaped.push('\\');
                }
                escaped.push(ch);
            }
            format!("\"${{HOME}}/{escaped}\"")
        }
        None => shell_single_quote(executable),
    }
}

fn shell_single_quote(path: &Path) -> String {
    let raw = path.to_string_lossy();
    format!("'{}'", raw.replace('\'', r#"'"'"'"#))
}

fn hook_command_variants(hook_script_path: &Path) -> Vec<String> {
    let mut variants = vec![HOOK_COMMAND_PATH.to_string()];
    let absolute = hook_script_path.to_string_lossy().into_owned();
    if absolute != HOOK_COMMAND_PATH {
        variants.push(absolute);
    }
    variants
}

fn ensure_settings_hooks(
    root: &mut Value,
    command_path: &str,
    command_variants: &[String],
) -> Result<Vec<String>> {
    let root_object =
        root.as_object_mut().ok_or_else(|| anyhow!("settings.json root must be a JSON object"))?;

    let hooks_value =
        root_object.entry("hooks".to_string()).or_insert_with(|| Value::Object(Map::new()));
    let hooks_object = hooks_value
        .as_object_mut()
        .ok_or_else(|| anyhow!("settings.json hooks must be a JSON object"))?;

    let mut updated_specs = Vec::new();
    for spec in HOOK_SPECS {
        if ensure_hook_entry(hooks_object, *spec, command_path, command_variants)? {
            updated_specs.push(spec.describe());
        }
    }

    Ok(updated_specs)
}

fn ensure_hook_entry(
    hooks_object: &mut Map<String, Value>,
    spec: HookSpec,
    command_path: &str,
    command_variants: &[String],
) -> Result<bool> {
    let event_value =
        hooks_object.entry(spec.event.to_string()).or_insert_with(|| Value::Array(Vec::new()));
    let event_array = event_value
        .as_array_mut()
        .ok_or_else(|| anyhow!("settings.json hooks.{} must be an array", spec.event))?;

    let mut insertion_index = None;

    for (index, entry) in event_array.iter_mut().enumerate() {
        let entry_object = entry
            .as_object_mut()
            .ok_or_else(|| anyhow!("settings.json hooks.{} entries must be objects", spec.event))?;

        if !matcher_matches(entry_object, spec.matcher)? {
            continue;
        }

        let hooks_value =
            entry_object.entry("hooks".to_string()).or_insert_with(|| Value::Array(Vec::new()));
        let hooks_array = hooks_value.as_array_mut().ok_or_else(|| {
            anyhow!("settings.json hooks.{}[].hooks must be an array", spec.event)
        })?;

        if hooks_array.iter().any(|hook| command_hook_matches(hook, command_variants)) {
            return Ok(false);
        }

        if insertion_index.is_none() {
            insertion_index = Some(index);
        }
    }

    let hook_value = json!({
        "type": "command",
        "command": command_path,
    });

    if let Some(index) = insertion_index {
        let entry_object = event_array[index]
            .as_object_mut()
            .ok_or_else(|| anyhow!("settings.json hooks.{} entries must be objects", spec.event))?;
        let hooks_array =
            entry_object.get_mut("hooks").and_then(Value::as_array_mut).ok_or_else(|| {
                anyhow!("settings.json hooks.{}[].hooks must be an array", spec.event)
            })?;
        hooks_array.push(hook_value);
        return Ok(true);
    }

    let new_entry = match spec.matcher {
        Some(matcher) => json!({
            "matcher": matcher,
            "hooks": [hook_value],
        }),
        None => json!({
            "hooks": [hook_value],
        }),
    };
    event_array.push(new_entry);
    Ok(true)
}

fn matcher_matches(entry_object: &Map<String, Value>, expected: Option<&str>) -> Result<bool> {
    let actual = match entry_object.get("matcher") {
        None => None,
        Some(Value::Null) => None,
        Some(Value::String(value)) => Some(value.as_str()),
        Some(_) => return Err(anyhow!("settings.json hook matcher must be a string")),
    };
    Ok(actual == expected)
}

fn command_hook_matches(hook: &Value, command_paths: &[String]) -> bool {
    let Some(hook_object) = hook.as_object() else {
        return false;
    };
    let hook_type = hook_object.get("type").and_then(Value::as_str);
    let command = hook_object.get("command").and_then(Value::as_str);
    hook_type == Some("command")
        && command
            .map(|value| command_paths.iter().any(|candidate| candidate == value))
            .unwrap_or(false)
}

pub fn uninstall_claude_hooks(claude_dir: &Path) -> Result<ClaudeUninstallResult> {
    let settings_path = claude_dir.join(SETTINGS_NAME);
    let hook_script_path = claude_dir.join("hooks").join(HOOK_SCRIPT_NAME);
    let command_variants = hook_command_variants(&hook_script_path);

    if !settings_path.exists() {
        let script_removed = match fs::remove_file(&hook_script_path) {
            Ok(()) => true,
            Err(err) if err.kind() == std::io::ErrorKind::NotFound => false,
            Err(err) => {
                return Err(err)
                    .with_context(|| format!("failed to remove {}", hook_script_path.display()));
            }
        };
        return Ok(ClaudeUninstallResult {
            settings_path: settings_path.to_string_lossy().into_owned(),
            settings_updated: false,
            script_removed,
        });
    }

    // Best-effort settings cleanup: if settings.json can't be read or parsed,
    // skip settings processing but still remove the hook script. Uninstall's
    // job is to clean up esp32dash artifacts, not to hard-fail on a corrupt
    // file the user may need to fix separately. Matches install-side's .ok()
    // tolerance for the hook script read.
    let settings_updated = match read_and_clean_settings(&settings_path, &command_variants) {
        Ok(updated) => updated,
        Err(err) => {
            eprintln!("warning: skipping settings cleanup for {}: {err}", settings_path.display());
            false
        }
    };

    let script_removed = match fs::remove_file(&hook_script_path) {
        Ok(()) => true,
        Err(err) if err.kind() == std::io::ErrorKind::NotFound => false,
        Err(err) => {
            return Err(err)
                .with_context(|| format!("failed to remove {}", hook_script_path.display()));
        }
    };

    Ok(ClaudeUninstallResult {
        settings_path: settings_path.to_string_lossy().into_owned(),
        settings_updated,
        script_removed,
    })
}

/// Reads settings.json, removes esp32dash hook entries, and writes back
/// atomically. Returns whether any entries were removed.
fn read_and_clean_settings(settings_path: &Path, command_variants: &[String]) -> Result<bool> {
    let raw = fs::read_to_string(settings_path)
        .with_context(|| format!("failed to read {}", settings_path.display()))?;
    let mut settings_json: Value = serde_json::from_str(&raw)
        .with_context(|| format!("failed to parse {}", settings_path.display()))?;
    let settings_updated = remove_esp32dash_hook_entries(&mut settings_json, command_variants)?;
    if settings_updated {
        atomic_write_json(settings_path, &settings_json)?;
    }
    Ok(settings_updated)
}

fn remove_esp32dash_hook_entries(root: &mut Value, command_variants: &[String]) -> Result<bool> {
    let Some(hooks_value) = root.as_object_mut().and_then(|object| object.get_mut("hooks")) else {
        return Ok(false);
    };
    let Some(hooks_object) = hooks_value.as_object_mut() else {
        return Ok(false);
    };

    let mut changed = false;
    let mut empty_events = Vec::new();

    for (event_name, event_value) in hooks_object.iter_mut() {
        let Some(event_array) = event_value.as_array_mut() else {
            continue;
        };

        let mut matcher_changed: Vec<bool> = Vec::with_capacity(event_array.len());
        for matcher_entry in event_array.iter_mut() {
            let Some(entry_object) = matcher_entry.as_object_mut() else {
                matcher_changed.push(false);
                continue;
            };
            let Some(hooks_array) = entry_object.get_mut("hooks").and_then(Value::as_array_mut)
            else {
                matcher_changed.push(false);
                continue;
            };
            let before = hooks_array.len();
            hooks_array.retain(|hook| !command_hook_matches(hook, command_variants));
            let removed = hooks_array.len() != before;
            if removed {
                changed = true;
            }
            matcher_changed.push(removed);
        }

        // Remove only matcher entries where esp32dash hooks were actually
        // removed AND the hooks array is now empty. Leave untouched matchers
        // (even if they have an empty or missing hooks array) alone.
        let before = event_array.len();
        let mut idx = 0;
        event_array.retain(|entry| {
            let changed_here = matcher_changed.get(idx).copied().unwrap_or(false);
            idx += 1;
            if !changed_here {
                return true;
            }
            entry
                .as_object()
                .and_then(|object| object.get("hooks"))
                .and_then(Value::as_array)
                .is_some_and(|hooks| !hooks.is_empty())
        });
        if event_array.len() != before {
            changed = true;
        }

        if event_array.is_empty() {
            empty_events.push(event_name.clone());
        }
    }

    for event_name in empty_events {
        hooks_object.remove(&event_name);
        changed = true;
    }

    // If the hooks object is now empty, remove it entirely.
    if hooks_object.is_empty() && root.as_object_mut().is_some_and(|o| o.remove("hooks").is_some())
    {
        changed = true;
    }

    Ok(changed)
}

fn ensure_executable(path: &Path) -> Result<()> {
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;

        let metadata =
            fs::metadata(path).with_context(|| format!("failed to stat {}", path.display()))?;
        let mut permissions = metadata.permissions();
        permissions.set_mode(0o755);
        fs::set_permissions(path, permissions)
            .with_context(|| format!("failed to chmod {}", path.display()))?;
    }

    Ok(())
}

impl HookSpec {
    fn describe(self) -> String {
        match self.matcher {
            Some(matcher) => format!("{} [{}]", self.event, matcher),
            None => self.event.to_string(),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{
        process,
        time::{SystemTime, UNIX_EPOCH},
    };

    fn temp_claude_dir(name: &str) -> PathBuf {
        let unique = SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default().as_nanos();
        std::env::temp_dir().join(format!("esp32dash-hooks-{name}-{}-{unique}", process::id()))
    }

    #[test]
    fn ensure_settings_hooks_initializes_empty_settings() {
        let mut root = Value::Object(Map::new());
        let updated =
            ensure_settings_hooks(&mut root, HOOK_COMMAND_PATH, &[HOOK_COMMAND_PATH.to_string()])
                .unwrap();

        assert_eq!(updated.len(), HOOK_SPECS.len());
        let hooks = root.get("hooks").and_then(Value::as_object).unwrap();
        assert!(hooks.contains_key("SessionStart"));
        assert!(hooks.contains_key("PermissionRequest"));
        assert!(hooks.contains_key("PreCompact"));
    }

    #[test]
    fn ensure_settings_hooks_is_idempotent() {
        let mut root = Value::Object(Map::new());
        let command_variants = [HOOK_COMMAND_PATH.to_string()];
        ensure_settings_hooks(&mut root, HOOK_COMMAND_PATH, &command_variants).unwrap();
        let updated =
            ensure_settings_hooks(&mut root, HOOK_COMMAND_PATH, &command_variants).unwrap();

        assert!(updated.is_empty());
    }

    #[test]
    fn ensure_settings_hooks_preserves_existing_hooks() {
        let mut root = json!({
            "hooks": {
                "PreToolUse": [
                    {
                        "matcher": "*",
                        "hooks": [
                            {
                                "type": "command",
                                "command": "~/.claude/hooks/notchi-hook.sh"
                            }
                        ]
                    }
                ]
            }
        });

        let command_variants = [HOOK_COMMAND_PATH.to_string()];
        let updated =
            ensure_settings_hooks(&mut root, HOOK_COMMAND_PATH, &command_variants).unwrap();
        assert!(updated.iter().any(|item| item == "PreToolUse [*]"));

        let hooks = root["hooks"]["PreToolUse"][0]["hooks"].as_array().unwrap();
        assert_eq!(hooks.len(), 2);
        assert!(hooks.iter().any(|hook| {
            command_hook_matches(hook, &["~/.claude/hooks/notchi-hook.sh".to_string()])
        }));
        assert!(
            hooks.iter().any(|hook| command_hook_matches(hook, &[HOOK_COMMAND_PATH.to_string()]))
        );
    }

    #[test]
    fn ensure_settings_hooks_accepts_absolute_installed_hook_path() {
        let absolute_path = "/tmp/.claude/hooks/esp32dash-hook.sh";
        let mut root = json!({
            "hooks": {
                "SessionStart": [
                    {
                        "hooks": [
                            {
                                "type": "command",
                                "command": absolute_path
                            }
                        ]
                    }
                ]
            }
        });

        let command_variants = [HOOK_COMMAND_PATH.to_string(), absolute_path.to_string()];
        let updated =
            ensure_settings_hooks(&mut root, HOOK_COMMAND_PATH, &command_variants).unwrap();

        assert!(!updated.iter().any(|item| item == "SessionStart"));
        let hooks = root["hooks"]["SessionStart"][0]["hooks"].as_array().unwrap();
        assert_eq!(hooks.len(), 1);
    }

    #[test]
    fn render_hook_script_sends_every_event_through_ingest() {
        let script = render_hook_script(Path::new("/tmp/esp32dash"));

        assert!(script.contains(r#""$BIN" claude ingest --event-from-stdin"#));
        assert!(script.trim_end().ends_with("exit 0"));
        assert!(!script.contains("respond"));
        assert!(!script.contains("PermissionRequest"));
    }

    #[test]
    fn shell_bin_path_uses_home_variable_under_home() {
        let home_dir = Path::new("/Users/me");
        assert_eq!(
            shell_bin_path(Path::new("/Users/me/.cargo/bin/esp32dash"), Some(home_dir)),
            r#""${HOME}/.cargo/bin/esp32dash""#
        );
        assert_eq!(
            shell_bin_path(Path::new("/Users/me/we$ird\"dir/esp32dash"), Some(home_dir)),
            r#""${HOME}/we\$ird\"dir/esp32dash""#
        );
        assert_eq!(
            shell_bin_path(Path::new("/opt/it's/esp32dash"), Some(home_dir)),
            r#"'/opt/it'"'"'s/esp32dash'"#
        );
    }

    #[test]
    fn analyze_install_detects_existing_state() {
        let claude_dir = temp_claude_dir("analyze");
        let hooks_dir = claude_dir.join("hooks");
        fs::create_dir_all(&hooks_dir).unwrap();

        let executable = PathBuf::from("/tmp/esp32dash");
        let expected_script = render_hook_script(&executable);
        let hook_script_path = hooks_dir.join(HOOK_SCRIPT_NAME);
        fs::write(&hook_script_path, &expected_script).unwrap();
        let mut settings = Value::Object(Map::new());
        ensure_settings_hooks(&mut settings, HOOK_COMMAND_PATH, &[HOOK_COMMAND_PATH.to_string()])
            .unwrap();
        fs::write(claude_dir.join(SETTINGS_NAME), serde_json::to_string_pretty(&settings).unwrap())
            .unwrap();

        let analysis = analyze_install(&claude_dir, &executable).unwrap();
        assert!(!analysis.script_written);
        assert!(!analysis.settings_updated);

        let _ = fs::remove_dir_all(&claude_dir);
    }

    #[test]
    fn uninstall_claude_removes_esp32dash_entries() {
        let claude_dir = temp_claude_dir("uninstall-claude");
        let hooks_dir = claude_dir.join("hooks");
        fs::create_dir_all(&hooks_dir).unwrap();

        let mut settings = Value::Object(Map::new());
        ensure_settings_hooks(&mut settings, HOOK_COMMAND_PATH, &[HOOK_COMMAND_PATH.to_string()])
            .unwrap();
        fs::write(claude_dir.join(SETTINGS_NAME), serde_json::to_string_pretty(&settings).unwrap())
            .unwrap();
        fs::write(hooks_dir.join(HOOK_SCRIPT_NAME), "#!/bin/sh\n").unwrap();

        let result = uninstall_claude_hooks(&claude_dir).unwrap();
        assert!(result.settings_updated);
        assert!(result.script_removed);

        let after = fs::read_to_string(claude_dir.join(SETTINGS_NAME)).unwrap();
        let after_json: Value = serde_json::from_str(&after).unwrap();
        assert!(
            after_json.get("hooks").is_none()
                || after_json["hooks"].as_object().unwrap().is_empty()
        );

        let _ = fs::remove_dir_all(&claude_dir);
    }

    #[test]
    fn uninstall_claude_preserves_other_hooks() {
        let claude_dir = temp_claude_dir("uninstall-preserve");
        let hooks_dir = claude_dir.join("hooks");
        fs::create_dir_all(&hooks_dir).unwrap();

        let mut settings = json!({
            "hooks": {
                "PreToolUse": [
                    {
                        "matcher": "*",
                        "hooks": [
                            {"type": "command", "command": "~/.claude/hooks/esp32dash-hook.sh"},
                            {"type": "command", "command": "~/.claude/hooks/other-hook.sh"}
                        ]
                    }
                ]
            }
        });
        let command_variants = [HOOK_COMMAND_PATH.to_string()];
        let _ = ensure_settings_hooks(&mut settings, HOOK_COMMAND_PATH, &command_variants).unwrap();
        fs::write(claude_dir.join(SETTINGS_NAME), serde_json::to_string_pretty(&settings).unwrap())
            .unwrap();

        let result = uninstall_claude_hooks(&claude_dir).unwrap();
        assert!(result.settings_updated);

        let after = fs::read_to_string(claude_dir.join(SETTINGS_NAME)).unwrap();
        let after_json: Value = serde_json::from_str(&after).unwrap();
        let hooks_array = after_json["hooks"]["PreToolUse"][0]["hooks"].as_array().unwrap();
        assert_eq!(hooks_array.len(), 1);
        assert_eq!(hooks_array[0]["command"], "~/.claude/hooks/other-hook.sh");

        let _ = fs::remove_dir_all(&claude_dir);
    }

    #[test]
    fn uninstall_claude_is_idempotent_on_clean_settings() {
        let claude_dir = temp_claude_dir("uninstall-idempotent");
        let result = uninstall_claude_hooks(&claude_dir).unwrap();
        assert!(!result.settings_updated);
        assert!(!result.script_removed);

        let _ = fs::remove_dir_all(&claude_dir);
    }

    #[test]
    fn uninstall_claude_succeeds_when_settings_missing() {
        let claude_dir = temp_claude_dir("uninstall-no-settings");
        let result = uninstall_claude_hooks(&claude_dir).unwrap();
        assert!(!result.settings_updated);
        assert!(!result.script_removed);

        let _ = fs::remove_dir_all(&claude_dir);
    }

    #[test]
    fn install_then_uninstall_round_trip() {
        let claude_dir = temp_claude_dir("round-trip");
        let hooks_dir = claude_dir.join("hooks");
        fs::create_dir_all(&hooks_dir).unwrap();

        let executable = PathBuf::from("/tmp/esp32dash");
        let expected_script = render_hook_script(&executable);
        fs::write(hooks_dir.join(HOOK_SCRIPT_NAME), &expected_script).unwrap();
        let mut settings = Value::Object(Map::new());
        ensure_settings_hooks(&mut settings, HOOK_COMMAND_PATH, &[HOOK_COMMAND_PATH.to_string()])
            .unwrap();
        let original = serde_json::to_string_pretty(&settings).unwrap();
        fs::write(claude_dir.join(SETTINGS_NAME), &original).unwrap();

        uninstall_claude_hooks(&claude_dir).unwrap();

        let after = fs::read_to_string(claude_dir.join(SETTINGS_NAME)).unwrap();
        let after_json: Value = serde_json::from_str(&after).unwrap();
        assert!(
            after_json.get("hooks").is_none()
                || after_json["hooks"].as_object().unwrap().is_empty()
        );
        assert!(!hooks_dir.join(HOOK_SCRIPT_NAME).exists());

        let _ = fs::remove_dir_all(&claude_dir);
    }
}
