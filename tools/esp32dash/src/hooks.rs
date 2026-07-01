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

const OMP_EXTENSION_NAME: &str = "esp32dash.ts";

#[derive(Debug, Clone, Serialize)]
pub struct OmpInstallResult {
    pub extension_path: String,
    pub extension_written: bool,
}

#[derive(Debug, Clone, Serialize)]
pub struct HooksResult {
    #[serde(skip_serializing_if = "Option::is_none")]
    pub claude: Option<InstallHooksResult>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub omp: Option<OmpInstallResult>,
}

#[derive(Debug, Clone, Serialize)]
pub struct ClaudeUninstallResult {
    pub settings_path: String,
    pub settings_updated: bool,
    pub script_removed: bool,
}

#[derive(Debug, Clone, Serialize)]
pub struct OmpUninstallResult {
    pub extension_path: String,
    pub extension_removed: bool,
}

#[derive(Debug, Clone, Serialize)]
pub struct UninstallHooksResult {
    #[serde(skip_serializing_if = "Option::is_none")]
    pub claude: Option<ClaudeUninstallResult>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub omp: Option<OmpUninstallResult>,
}

#[derive(Debug)]
struct OmpAnalysis {
    extension_path: PathBuf,
    extension_contents: String,
    extension_written: bool,
}

fn home_claude_dir() -> Result<PathBuf> {
    Ok(home().ok_or_else(|| anyhow!("failed to resolve home directory"))?.join(CLAUDE_DIR_NAME))
}

pub fn install_hooks(
    executable: &Path,
    force: bool,
    install_claude: bool,
    install_omp: bool,
) -> Result<HooksResult> {
    let claude_dir = if install_claude {
        Some(home_claude_dir()?)
    } else {
        None
    };
    let claude_analysis = if let Some(dir) = claude_dir.as_deref() {
        Some(analyze_install(dir, executable)?)
    } else {
        None
    };

    let omp_analysis = if install_omp {
        let omp_dir = resolve_omp_agent_dir()?;
        Some(analyze_omp(&omp_dir)?)
    } else {
        None
    };

    let claude_needs_write =
        claude_analysis.as_ref().is_some_and(|a| a.script_written || a.settings_updated);
    let omp_needs_write = omp_analysis.as_ref().is_some_and(|analysis| analysis.extension_written);

    if !claude_needs_write && !omp_needs_write {
        return Ok(HooksResult {
            claude: claude_analysis.map(|analysis| InstallHooksResult {
                ok: true,
                hook_script_path: analysis.hook_script_path.to_string_lossy().into_owned(),
                settings_path: analysis.settings_path.to_string_lossy().into_owned(),
                script_written: false,
                settings_updated: false,
                changes_applied: false,
            }),
            omp: omp_analysis.map(|analysis| OmpInstallResult {
                extension_path: analysis.extension_path.to_string_lossy().into_owned(),
                extension_written: false,
            }),
        });
    }

    if !force {
        let confirm_message =
            build_combined_confirm_message(claude_analysis.as_ref(), omp_analysis.as_ref());
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

    if let (Some(dir), Some(analysis)) = (claude_dir.as_deref(), claude_analysis.as_ref()) {
        apply_install(dir, analysis)?;
    }

    if let Some(analysis) = omp_analysis.as_ref() {
        apply_omp(analysis)?;
    }

    Ok(HooksResult {
        claude: claude_analysis.map(|analysis| InstallHooksResult {
            ok: true,
            hook_script_path: analysis.hook_script_path.to_string_lossy().into_owned(),
            settings_path: analysis.settings_path.to_string_lossy().into_owned(),
            script_written: analysis.script_written,
            settings_updated: analysis.settings_updated,
            changes_applied: analysis.script_written || analysis.settings_updated,
        }),
        omp: omp_analysis.map(|analysis| OmpInstallResult {
            extension_path: analysis.extension_path.to_string_lossy().into_owned(),
            extension_written: analysis.extension_written,
        }),
    })
}

pub fn uninstall_hooks(
    uninstall_claude: bool,
    uninstall_omp: bool,
) -> Result<UninstallHooksResult> {
    let claude_result = if uninstall_claude {
        let claude_dir = home_claude_dir()?;
        Some(uninstall_claude_hooks(&claude_dir)?)
    } else {
        None
    };

    let omp_result = if uninstall_omp {
        let omp_dir = resolve_omp_agent_dir()?;
        Some(uninstall_omp_hooks(&omp_dir)?)
    } else {
        None
    };

    Ok(UninstallHooksResult {
        claude: claude_result,
        omp: omp_result,
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

fn build_combined_confirm_message(
    claude: Option<&InstallAnalysis>,
    omp: Option<&OmpAnalysis>,
) -> String {
    let mut lines = Vec::new();
    lines.push("Install esp32dash hooks?".to_string());
    if let Some(claude) = claude {
        if claude.script_written {
            lines.push(format!("Write hook script: {}", claude.hook_script_path.display()));
        }
        if claude.settings_updated {
            lines.push(format!("Update settings: {}", claude.settings_path.display()));
            lines.push(format!("Add hook entries: {}", claude.updated_specs.join(", ")));
        }
    }
    if let Some(analysis) = omp.filter(|a| a.extension_written) {
        lines.push(format!("Write OMP extension: {}", analysis.extension_path.display()));
    }
    lines.join("\n")
}

fn resolve_omp_agent_dir() -> Result<PathBuf> {
    use std::env;
    if let Some(dir) = env::var_os("OMP_AGENT_DIR") {
        return Ok(PathBuf::from(dir));
    }
    if let Some(dir) = env::var_os("PI_CODING_AGENT_DIR") {
        return Ok(PathBuf::from(dir));
    }
    Ok(home()
        .ok_or_else(|| anyhow!("failed to resolve home directory"))?
        .join(".omp")
        .join("agent"))
}

fn analyze_omp(omp_dir: &Path) -> Result<OmpAnalysis> {
    let extensions_dir = omp_dir.join("extensions");
    let extension_path = extensions_dir.join(OMP_EXTENSION_NAME);
    let extension_contents = render_omp_extension();
    let existing = fs::read_to_string(&extension_path).ok();
    let extension_written = existing.as_deref() != Some(extension_contents.as_str());
    Ok(OmpAnalysis {
        extension_path,
        extension_contents,
        extension_written,
    })
}

fn apply_omp(analysis: &OmpAnalysis) -> Result<()> {
    if !analysis.extension_written {
        return Ok(());
    }
    if let Some(parent) = analysis.extension_path.parent() {
        fs::create_dir_all(parent)
            .with_context(|| format!("failed to create {}", parent.display()))?;
    }
    fs::write(&analysis.extension_path, &analysis.extension_contents).with_context(|| {
        format!("failed to write extension {}", analysis.extension_path.display())
    })?;
    Ok(())
}

pub fn uninstall_omp_hooks(omp_dir: &Path) -> Result<OmpUninstallResult> {
    let extension_path = omp_dir.join("extensions").join(OMP_EXTENSION_NAME);
    let extension_removed = match fs::remove_file(&extension_path) {
        Ok(()) => true,
        Err(err) if err.kind() == std::io::ErrorKind::NotFound => false,
        Err(err) => {
            return Err(err).with_context(|| {
                format!("failed to remove extension {}", extension_path.display())
            });
        }
    };
    Ok(OmpUninstallResult {
        extension_path: extension_path.to_string_lossy().into_owned(),
        extension_removed,
    })
}

fn render_omp_extension() -> String {
    // Use the compile-time constant as the fallback, not the env-derived
    // admin_addr() — a misconfigured ESP32DASH_ADMIN_ADDR (quotes, backticks)
    // could break the rendered JS string literal. The runtime process.env
    // read in the extension carries any legitimate override.
    let default_addr = crate::compat::DEFAULT_ADMIN_ADDR;
    format!(
        r#"// Auto-generated by esp32dash `hooks install omp`.
// Forwards OMP session/turn/tool lifecycle events to the esp32dash agent,
// translated to Claude-compatible hook_event_name values so the device
// displays a unified agent status (last-event-wins).
const ENDPOINT =
  `http://${{process.env.ESP32DASH_ADMIN_ADDR || "{default_addr}"}}/v1/claude/events`;

function sessionId(ctx: any): string {{
  try {{
    const sm = ctx?.sessionManager;
    const id =
      (typeof sm?.getSessionFile === "function" && sm.getSessionFile()) ||
      (typeof sm?.sessionId === "string" && sm.sessionId) ||
      undefined;
    if (id) return String(id);
  }} catch {{
    /* fall through to placeholder */
  }}
  return "omp-session";
}}

async function postEvent(payload: Record<string, unknown>): Promise<void> {{
  try {{
    await fetch(ENDPOINT, {{
      method: "POST",
      headers: {{ "Content-Type": "application/json" }},
      body: JSON.stringify(payload),
    }});
  }} catch (err) {{
    // Agent unreachable — fail silently, do not block OMP.
    console.error("[esp32dash] event post failed:", err);
  }}
}}

function buildEvent(
  ctx: any,
  hookEventName: string,
  toolName?: string,
): Record<string, unknown> {{
  return {{
    session_id: sessionId(ctx),
    cwd: String(ctx?.cwd ?? process.cwd()),
    hook_event_name: hookEventName,
    tool_name: toolName,
    permission_mode: "default",
    recv_ts: Math.floor(Date.now() / 1000),
  }};
}}

export default function (pi: any): void {{
  pi.on("session_start", async (_event: unknown, ctx: any) => {{
    await postEvent(buildEvent(ctx, "SessionStart"));
  }});

  pi.on("session_shutdown", async (_event: unknown, ctx: any) => {{
    await postEvent(buildEvent(ctx, "SessionEnd"));
  }});

  pi.on("turn_start", async (_event: unknown, ctx: any) => {{
    await postEvent(buildEvent(ctx, "UserPromptSubmit"));
  }});

  pi.on("turn_end", async (_event: unknown, ctx: any) => {{
    await postEvent(buildEvent(ctx, "Stop"));
  }});

  pi.on("tool_call", async (event: any, ctx: any) => {{
    const toolName = typeof event?.toolName === "string" ? event.toolName : undefined;
    await postEvent(buildEvent(ctx, "PreToolUse", toolName));
  }});
}}
"#
    )
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

# Route user-input events through the blocking respond path
EVENT_NAME=$(printf '%s' "$INPUT" | grep -o '"hook_event_name"[[:space:]]*:[[:space:]]*"[^"]*"' | head -1 | sed 's/.*"hook_event_name"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/')
TOOL_NAME=$(printf '%s' "$INPUT" | grep -o '"tool_name"[[:space:]]*:[[:space:]]*"[^"]*"' | head -1 | sed 's/.*"tool_name"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/')

IS_RESPOND_EVENT=0
if [ "$EVENT_NAME" = "PermissionRequest" ]; then
  IS_RESPOND_EVENT=1
fi

if [ "$IS_RESPOND_EVENT" = "1" ]; then
  printf '%s' "$INPUT" | exec "$BIN" claude respond --event-from-stdin
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
    fn render_hook_script_routes_only_permission_request_to_respond() {
        let script = render_hook_script(Path::new("/tmp/esp32dash"));

        // PermissionRequest must route to respond
        assert!(
            script.contains(r#"[ "$EVENT_NAME" = "PermissionRequest" ]"#),
            "script must contain PermissionRequest condition"
        );

        // Elicitation must NOT appear in the IS_RESPOND_EVENT block
        let respond_block_start = script.find("IS_RESPOND_EVENT=0").unwrap();
        let respond_block_end = script.find("if [ \"$IS_RESPOND_EVENT\" = \"1\" ]").unwrap();
        let respond_block = &script[respond_block_start..respond_block_end];
        assert!(
            !respond_block.contains("Elicitation"),
            "Elicitation must not appear in the IS_RESPOND_EVENT logic"
        );
        assert!(
            !respond_block.contains("AskUserQuestion"),
            "AskUserQuestion must not appear in the IS_RESPOND_EVENT logic"
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

    // --- OMP extension rendering tests ---

    #[test]
    fn render_omp_extension_contains_factory_and_handlers() {
        let src = render_omp_extension();
        assert!(src.contains("export default function"), "missing factory export");
        assert!(src.contains("pi.on(\"session_start\""));
        assert!(src.contains("pi.on(\"session_shutdown\""));
        assert!(src.contains("pi.on(\"turn_start\""));
        assert!(src.contains("pi.on(\"turn_end\""));
        assert!(src.contains("pi.on(\"tool_call\""));
        assert!(src.contains("fetch("));
    }

    #[test]
    fn render_omp_extension_contains_all_claude_event_literals() {
        let src = render_omp_extension();
        for literal in [
            "\"SessionStart\"",
            "\"SessionEnd\"",
            "\"UserPromptSubmit\"",
            "\"Stop\"",
            "\"PreToolUse\"",
        ] {
            assert!(
                src.contains(literal),
                "rendered extension must contain Claude event literal {literal}"
            );
        }
    }

    #[test]
    fn render_omp_extension_uses_epoch_seconds_not_millis() {
        let src = render_omp_extension();
        assert!(
            src.contains("Math.floor(Date.now() / 1000)"),
            "recv_ts must use epoch seconds (Math.floor(Date.now() / 1000)), not raw millis"
        );
        // Ensure no bare Date.now() call that isn't divided.
        let bare = src.matches("Date.now()").count();
        let divided = src.matches("Date.now() / 1000").count();
        assert_eq!(
            bare, divided,
            "all Date.now() calls must be divided by 1000 to produce seconds"
        );
    }

    #[test]
    fn render_omp_extension_includes_default_admin_addr() {
        let src = render_omp_extension();
        assert!(
            src.contains(crate::compat::DEFAULT_ADMIN_ADDR),
            "extension must contain default admin addr as fallback"
        );
        assert!(
            src.contains("ESP32DASH_ADMIN_ADDR"),
            "extension must read ESP32DASH_ADMIN_ADDR env override"
        );
    }

    #[test]
    fn render_omp_extension_permission_mode_is_default() {
        let src = render_omp_extension();
        assert!(
            src.contains("permission_mode: \"default\""),
            "OMP extension must set permission_mode to \"default\" (normalizer safe fallback)"
        );
    }

    #[test]
    fn omp_payload_round_trips_through_normalizer() {
        // Verify the JSON payload the extension would POST is deserializable as
        // LocalHookEvent and produces the expected snapshot title/status.
        use crate::model::{LocalHookEvent, Snapshot};
        use crate::normalizer::normalize;

        let src = render_omp_extension();
        assert!(src.contains("\"PreToolUse\""));

        // Construct the payload the tool_call handler would build.
        let payload = json!({
            "session_id": "omp-sess-1",
            "cwd": "/tmp/project",
            "hook_event_name": "PreToolUse",
            "tool_name": "Write",
            "permission_mode": "default",
            "recv_ts": 1700000000_u64,
        });
        let event: LocalHookEvent = serde_json::from_value(payload).unwrap();
        assert_eq!(event.hook_event_name, "PreToolUse");

        let snap = normalize(&event, &Snapshot::empty(1));
        assert_eq!(snap.status, "running_tool");
        assert_eq!(snap.detail, "Write");
    }

    // --- OMP install/uninstall filesystem tests ---

    fn temp_omp_dir(name: &str) -> PathBuf {
        let unique = SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default().as_nanos();
        std::env::temp_dir().join(format!("esp32dash-omp-{name}-{}-{unique}", process::id()))
    }

    #[test]
    fn analyze_omp_detects_missing_extension() {
        let omp_dir = temp_omp_dir("analyze");
        let analysis = analyze_omp(&omp_dir).unwrap();
        assert!(analysis.extension_written);
        assert_eq!(analysis.extension_path, omp_dir.join("extensions").join(OMP_EXTENSION_NAME));

        let _ = fs::remove_dir_all(&omp_dir);
    }

    #[test]
    fn apply_omp_writes_extension_file() {
        let omp_dir = temp_omp_dir("apply");
        let analysis = analyze_omp(&omp_dir).unwrap();
        apply_omp(&analysis).unwrap();

        assert!(analysis.extension_path.exists());
        let written = fs::read_to_string(&analysis.extension_path).unwrap();
        assert!(written.contains("export default function"));

        let _ = fs::remove_dir_all(&omp_dir);
    }

    #[test]
    fn analyze_omp_is_idempotent_when_content_matches() {
        let omp_dir = temp_omp_dir("idempotent");
        let first = analyze_omp(&omp_dir).unwrap();
        apply_omp(&first).unwrap();

        let second = analyze_omp(&omp_dir).unwrap();
        assert!(!second.extension_written, "re-analyzing after install must be no-op");

        let _ = fs::remove_dir_all(&omp_dir);
    }

    #[test]
    fn uninstall_omp_removes_extension_file() {
        let omp_dir = temp_omp_dir("uninstall");
        let analysis = analyze_omp(&omp_dir).unwrap();
        apply_omp(&analysis).unwrap();
        assert!(analysis.extension_path.exists());

        let result = uninstall_omp_hooks(&omp_dir).unwrap();
        assert!(result.extension_removed);
        assert!(!analysis.extension_path.exists());

        let _ = fs::remove_dir_all(&omp_dir);
    }

    #[test]
    fn uninstall_omp_is_idempotent_when_missing() {
        let omp_dir = temp_omp_dir("uninstall-missing");
        let result = uninstall_omp_hooks(&omp_dir).unwrap();
        assert!(!result.extension_removed);

        let _ = fs::remove_dir_all(&omp_dir);
    }

    #[test]
    fn install_omp_then_uninstall_round_trip() {
        let omp_dir = temp_omp_dir("round-trip");
        let analysis = analyze_omp(&omp_dir).unwrap();
        apply_omp(&analysis).unwrap();
        assert!(analysis.extension_path.exists());

        uninstall_omp_hooks(&omp_dir).unwrap();
        assert!(!analysis.extension_path.exists());

        let _ = fs::remove_dir_all(&omp_dir);
    }

    #[test]
    fn omp_install_does_not_touch_claude_hooks() {
        // Installing OMP extension should not create or modify any Claude settings.
        let omp_dir = temp_omp_dir("independence-omp");
        let claude_dir = temp_claude_dir("independence-omp");
        fs::create_dir_all(&claude_dir).unwrap();

        let analysis = analyze_omp(&omp_dir).unwrap();
        apply_omp(&analysis).unwrap();

        assert!(!claude_dir.join(SETTINGS_NAME).exists());
        assert!(!claude_dir.join("hooks").exists());
        assert!(analysis.extension_path.exists());

        let _ = fs::remove_dir_all(&omp_dir);
        let _ = fs::remove_dir_all(&claude_dir);
    }

    #[test]
    fn install_hooks_omits_claude_when_target_is_omp_only() {
        // R8: requesting only OMP must not analyze or apply Claude. When
        // install_claude is false the coordinator skips home_claude_dir entirely,
        // so the result carries no Claude entry. This would fail on the old
        // coordinator which always set claude = Some(...).
        let exe = PathBuf::from("/tmp/esp32dash");
        let result = install_hooks(&exe, true, false, false).unwrap();
        assert!(result.claude.is_none(), "Claude result must be absent when install_claude=false");
        assert!(result.omp.is_none(), "OMP result must be absent when install_omp=false");
    }

    #[test]
    fn uninstall_hooks_omits_claude_when_target_is_omp_only() {
        // R8: uninstalling only OMP must not touch Claude hooks. The old
        // coordinator always called uninstall_claude_hooks and returned
        // claude = Some(...); this assertion locks in the gate.
        let result = uninstall_hooks(false, false).unwrap();
        assert!(
            result.claude.is_none(),
            "Claude result must be absent when uninstall_claude=false"
        );
        assert!(result.omp.is_none(), "OMP result must be absent when uninstall_omp=false");
    }
}
