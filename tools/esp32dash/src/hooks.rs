use std::{
    fs,
    path::{Path, PathBuf},
};

use anyhow::{Context, Result, anyhow};
use directories::BaseDirs;
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

pub fn install_hooks(executable: &Path, force: bool) -> Result<InstallHooksResult> {
    let base_dirs = BaseDirs::new().context("failed to resolve home directory")?;
    let claude_dir = base_dirs.home_dir().join(CLAUDE_DIR_NAME);
    let analysis = analyze_install(&claude_dir, executable)?;

    if !analysis.script_written && !analysis.settings_updated {
        return Ok(InstallHooksResult {
            ok: true,
            hook_script_path: analysis.hook_script_path.to_string_lossy().into_owned(),
            settings_path: analysis.settings_path.to_string_lossy().into_owned(),
            script_written: false,
            settings_updated: false,
            changes_applied: false,
        });
    }

    if !force {
        let confirm_message = build_confirm_message(&analysis);
        let confirmed = match Confirm::new(&confirm_message).with_default(true).prompt() {
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

    apply_install(&claude_dir, &analysis)?;

    Ok(InstallHooksResult {
        ok: true,
        hook_script_path: analysis.hook_script_path.to_string_lossy().into_owned(),
        settings_path: analysis.settings_path.to_string_lossy().into_owned(),
        script_written: analysis.script_written,
        settings_updated: analysis.settings_updated,
        changes_applied: analysis.script_written || analysis.settings_updated,
    })
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

fn apply_install(claude_dir: &Path, analysis: &InstallAnalysis) -> Result<()> {
    let hooks_dir = claude_dir.join("hooks");
    fs::create_dir_all(&hooks_dir)
        .with_context(|| format!("failed to create {}", hooks_dir.display()))?;

    if analysis.script_written {
        fs::write(&analysis.hook_script_path, &analysis.hook_script_contents).with_context(
            || {
                format!(
                    "failed to write hook script {}",
                    analysis.hook_script_path.display()
                )
            },
        )?;
    }
    ensure_executable(&analysis.hook_script_path)?;

    if analysis.settings_updated {
        let formatted = serde_json::to_string_pretty(&analysis.settings_json)
            .context("failed to serialize updated settings.json")?;
        fs::write(&analysis.settings_path, format!("{formatted}\n"))
            .with_context(|| format!("failed to write {}", analysis.settings_path.display()))?;
    }

    Ok(())
}

fn build_confirm_message(analysis: &InstallAnalysis) -> String {
    let mut lines = Vec::new();
    lines.push("Install esp32dash Claude hooks?".to_string());
    if analysis.script_written {
        lines.push(format!(
            "Write hook script: {}",
            analysis.hook_script_path.display()
        ));
    }
    if analysis.settings_updated {
        lines.push(format!(
            "Update settings: {}",
            analysis.settings_path.display()
        ));
        lines.push(format!(
            "Add hook entries: {}",
            analysis.updated_specs.join(", ")
        ));
    }
    lines.join("\n")
}

fn render_hook_script(executable: &Path) -> String {
    let escaped_executable = shell_single_quote(executable);
    format!(
        r#"#!/bin/sh

DEFAULT_BIN={escaped_executable}
BIN="${{ESP32DASH_BIN:-$DEFAULT_BIN}}"
export ESP32DASH_CLAUDE_PID="${{PPID:-}}"

if [ ! -x "$BIN" ]; then
  if command -v esp32dash >/dev/null 2>&1; then
    BIN=esp32dash
  else
    exit 0
  fi
fi

# Read stdin so we can inspect and re-pipe it
INPUT=$(cat)

# PermissionRequest needs the blocking approve path
EVENT_NAME=$(printf '%s' "$INPUT" | grep -o '"hook_event_name"[[:space:]]*:[[:space:]]*"[^"]*"' | head -1 | sed 's/.*"hook_event_name"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/')

if [ "$EVENT_NAME" = "PermissionRequest" ]; then
  printf '%s' "$INPUT" | exec "$BIN" claude approve --event-from-stdin
else
  printf '%s' "$INPUT" | "$BIN" claude ingest --event-from-stdin
  exit 0
fi
"#
    )
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
    let root_object = root
        .as_object_mut()
        .ok_or_else(|| anyhow!("settings.json root must be a JSON object"))?;

    let hooks_value = root_object
        .entry("hooks".to_string())
        .or_insert_with(|| Value::Object(Map::new()));
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
    let event_value = hooks_object
        .entry(spec.event.to_string())
        .or_insert_with(|| Value::Array(Vec::new()));
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

        let hooks_value = entry_object
            .entry("hooks".to_string())
            .or_insert_with(|| Value::Array(Vec::new()));
        let hooks_array = hooks_value.as_array_mut().ok_or_else(|| {
            anyhow!(
                "settings.json hooks.{}[].hooks must be an array",
                spec.event
            )
        })?;

        if hooks_array
            .iter()
            .any(|hook| command_hook_matches(hook, command_variants))
        {
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
        let hooks_array = entry_object
            .get_mut("hooks")
            .and_then(Value::as_array_mut)
            .ok_or_else(|| {
                anyhow!(
                    "settings.json hooks.{}[].hooks must be an array",
                    spec.event
                )
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
        let unique = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap_or_default()
            .as_nanos();
        std::env::temp_dir().join(format!("esp32dash-hooks-{name}-{}-{unique}", process::id()))
    }

    #[test]
    fn ensure_settings_hooks_initializes_empty_settings() {
        let mut root = Value::Object(Map::new());
        let updated = ensure_settings_hooks(
            &mut root,
            HOOK_COMMAND_PATH,
            &[HOOK_COMMAND_PATH.to_string()],
        )
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
            hooks
                .iter()
                .any(|hook| command_hook_matches(hook, &[HOOK_COMMAND_PATH.to_string()]))
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
        let hooks = root["hooks"]["SessionStart"][0]["hooks"]
            .as_array()
            .unwrap();
        assert_eq!(hooks.len(), 1);
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
        ensure_settings_hooks(
            &mut settings,
            HOOK_COMMAND_PATH,
            &[HOOK_COMMAND_PATH.to_string()],
        )
        .unwrap();
        fs::write(
            claude_dir.join(SETTINGS_NAME),
            serde_json::to_string_pretty(&settings).unwrap(),
        )
        .unwrap();

        let analysis = analyze_install(&claude_dir, &executable).unwrap();
        assert!(!analysis.script_written);
        assert!(!analysis.settings_updated);

        let _ = fs::remove_dir_all(&claude_dir);
    }
}
