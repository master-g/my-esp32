use crate::{
    model::{Attention, LocalHookEvent, RunStatus, Snapshot},
    text_sanitize::{build_workspace, sanitize_snapshot_detail, sanitize_snapshot_title},
};

pub fn normalize(event: &LocalHookEvent, current: &Snapshot) -> Snapshot {
    let workspace = build_workspace(&event.cwd);
    let mut status = map_status(&event.hook_event_name);
    let mut unread = false;
    let mut attention = Attention::Low;
    let permission_mode = event.permission_mode.clone();

    let (title, detail) = match event.hook_event_name.as_str() {
        "SessionStart" => ("Session started".to_string(), "Workspace available".to_string()),
        "UserPromptSubmit" => (
            "Processing prompt".to_string(),
            event.prompt_preview.clone().unwrap_or_else(|| "User prompt received".to_string()),
        ),
        "PreToolUse" => {
            if event.tool_name.as_deref() == Some("AskUserQuestion") {
                status = RunStatus::WaitingForInput;
                unread = true;
                attention = Attention::High;
                (
                    "Awaiting input".to_string(),
                    event
                        .waiting_prompt
                        .as_ref()
                        .map(|prompt| prompt.question.clone())
                        .unwrap_or_else(|| "Question pending in terminal".to_string()),
                )
            } else {
                (
                    "Running tool".to_string(),
                    event.tool_name.as_deref().unwrap_or("Tool execution started").to_string(),
                )
            }
        }
        "PostToolUse" => (
            "Tool finished".to_string(),
            event.tool_name.as_deref().unwrap_or("Tool execution completed").to_string(),
        ),
        "PostToolUseFailure" => (
            "Tool failed".to_string(),
            event
                .message
                .clone()
                .or_else(|| event.tool_name.clone())
                .unwrap_or_else(|| "Tool execution failed".to_string()),
        ),
        "PermissionRequest" => {
            unread = true;
            attention = Attention::High;
            (
                "Awaiting approval".to_string(),
                format!(
                    "{} requires approval ({})",
                    event.tool_name.as_deref().unwrap_or("Tool"),
                    permission_mode
                ),
            )
        }
        "PermissionDenied" => {
            unread = true;
            attention = Attention::Medium;
            (
                "Permission denied".to_string(),
                event
                    .message
                    .clone()
                    .or_else(|| event.tool_name.clone())
                    .unwrap_or_else(|| "Tool execution was denied".to_string()),
            )
        }
        "PreCompact" => ("Compacting context".to_string(), "Preparing context window".to_string()),
        "PostCompact" => ("Compaction finished".to_string(), "Continuing work".to_string()),
        "Elicitation" => {
            unread = true;
            attention = Attention::High;
            (
                "Awaiting input".to_string(),
                event
                    .waiting_prompt
                    .as_ref()
                    .map(|prompt| prompt.question.clone())
                    .or_else(|| event.message.clone())
                    .unwrap_or_else(|| "MCP server requested input".to_string()),
            )
        }
        "ElicitationResult" => (
            "Input received".to_string(),
            event
                .message
                .clone()
                .or_else(|| event.waiting_prompt.as_ref().map(|prompt| prompt.title.clone()))
                .unwrap_or_else(|| "Continuing after user input".to_string()),
        ),
        "SubagentStart" => (
            "Subagent running".to_string(),
            event.message.as_deref().unwrap_or("Delegated task in progress").to_string(),
        ),
        "Stop" => {
            unread = true;
            attention = Attention::Medium;
            (
                "Idle".to_string(),
                event.message.as_deref().unwrap_or("Previous action completed").to_string(),
            )
        }
        "SubagentStop" => {
            status = preserve_activity_status(current, RunStatus::Processing);
            (
                "Subagent finished".to_string(),
                event.message.as_deref().unwrap_or("Latest subagent task completed").to_string(),
            )
        }
        "SessionEnd" => {
            unread = true;
            attention = Attention::Medium;
            ("Session ended".to_string(), "Workspace session ended".to_string())
        }
        "StopFailure" => {
            unread = true;
            attention = Attention::High;
            (
                "Response failed".to_string(),
                event
                    .message
                    .as_deref()
                    .unwrap_or("Claude stopped due to an API or runtime error")
                    .to_string(),
            )
        }
        "Notification" => normalize_notification(event, current, &workspace),
        other => {
            status = RunStatus::Unknown;
            ("Unknown event".to_string(), other.to_string())
        }
    };

    Snapshot {
        seq: current.seq,
        source: "claude_code".to_string(),
        session_id: event.session_id.clone(),
        event: event.hook_event_name.clone(),
        status: status.as_str().to_string(),
        title: sanitize_snapshot_title(&title),
        workspace,
        detail: sanitize_snapshot_detail(&detail),
        permission_mode,
        ts: event.recv_ts,
        unread,
        attention,
    }
}

fn preserve_activity_status(current: &Snapshot, fallback: RunStatus) -> RunStatus {
    let current = RunStatus::from_str(&current.status);
    if current.is_active() {
        current
    } else {
        fallback
    }
}

fn normalize_notification(
    event: &LocalHookEvent,
    current: &Snapshot,
    workspace: &str,
) -> (String, String) {
    let message = event
        .message
        .as_deref()
        .or(event.prompt_preview.as_deref())
        .unwrap_or("Notification received")
        .to_string();

    let lowered = message.to_ascii_lowercase();
    if lowered.contains("approval") || lowered.contains("permission") {
        return ("Awaiting approval".to_string(), message);
    }
    if lowered.contains("waiting for input") || lowered.contains("awaiting input") {
        return ("Ready for input".to_string(), message);
    }

    let prefix = if workspace.is_empty() {
        "Notification"
    } else {
        "Notification"
    };
    let _ = current;
    (prefix.to_string(), message)
}

fn map_status(event: &str) -> RunStatus {
    match event {
        "UserPromptSubmit" => RunStatus::Processing,
        "PreCompact" => RunStatus::Compacting,
        "PostCompact" => RunStatus::Processing,
        "SessionStart" => RunStatus::Unknown,
        "SessionEnd" => RunStatus::Ended,
        "PreToolUse" => RunStatus::RunningTool,
        "PostToolUse" => RunStatus::Processing,
        "PostToolUseFailure" => RunStatus::Processing,
        "PermissionRequest" => RunStatus::WaitingForInput,
        "PermissionDenied" => RunStatus::WaitingForInput,
        "Elicitation" => RunStatus::WaitingForInput,
        "ElicitationResult" => RunStatus::Processing,
        "SubagentStart" => RunStatus::Processing,
        "Stop" => RunStatus::Unknown,
        "StopFailure" => RunStatus::WaitingForInput,
        "SubagentStop" => RunStatus::Processing,
        "Notification" => RunStatus::Unknown,
        _ => RunStatus::Unknown,
    }
}

pub fn apply_notification_status(
    mut snapshot: Snapshot,
    current: &Snapshot,
    event: &LocalHookEvent,
) -> Snapshot {
    if event.hook_event_name != "Notification" {
        return snapshot;
    }

    let lowered = snapshot.detail.to_ascii_lowercase();
    if lowered.contains("approval") || lowered.contains("permission") {
        snapshot.status = RunStatus::WaitingForInput.as_str().to_string();
        snapshot.unread = true;
        snapshot.attention = Attention::High;
        return snapshot;
    }
    if lowered.contains("waiting for input") || lowered.contains("awaiting input") {
        snapshot.status = RunStatus::WaitingForInput.as_str().to_string();
        snapshot.unread = true;
        snapshot.attention = Attention::Medium;
        return snapshot;
    }

    snapshot.status = current.status.clone();
    snapshot.unread = false;
    snapshot.attention = Attention::Low;
    snapshot
}

pub fn materially_equal(a: &Snapshot, b: &Snapshot) -> bool {
    a.status == b.status
        && a.title == b.title
        && a.detail == b.detail
        && a.workspace == b.workspace
        && a.session_id == b.session_id
        && a.unread == b.unread
        && a.attention == b.attention
        && a.permission_mode == b.permission_mode
}

#[cfg(test)]
mod tests {
    use crate::model::Snapshot;

    use super::{apply_notification_status, build_workspace, materially_equal, normalize};
    use crate::{
        model::{LocalHookEvent, WaitingPrompt, WaitingPromptKind, WaitingPromptOption},
        text_sanitize::sanitize_prompt_preview,
    };

    #[test]
    fn prompt_preview_is_single_line_and_truncated() {
        let prompt = "hello world\nsecond line";
        assert_eq!(sanitize_prompt_preview(prompt).as_deref(), Some("hello world"));
    }

    #[test]
    fn workspace_uses_last_path_component() {
        assert_eq!(build_workspace("/tmp/project-foo"), "project-foo");
    }

    #[test]
    fn permission_request_marks_unread() {
        let current = Snapshot::empty(1);
        let event = LocalHookEvent {
            session_id: "sess".into(),
            cwd: "/tmp/project".into(),
            hook_event_name: "PermissionRequest".into(),
            message: None,
            prompt_preview: None,
            tool_name: Some("exec_command".into()),
            tool_use_id: Some("tool-1".into()),
            permission_mode: "default".into(),
            waiting_prompt: None,
            recv_ts: 10,
            claude_pid: None,
        };

        let snapshot = normalize(&event, &current);
        assert!(snapshot.unread);
        assert_eq!(snapshot.status, "waiting_for_input");
    }

    #[test]
    fn notification_keeps_current_status_when_non_actionable() {
        let mut current = Snapshot::empty(1);
        current.status = "processing".into();
        let event = LocalHookEvent {
            session_id: "sess".into(),
            cwd: "/tmp/project".into(),
            hook_event_name: "Notification".into(),
            message: Some("Background summary".into()),
            prompt_preview: None,
            tool_name: None,
            tool_use_id: None,
            permission_mode: "default".into(),
            waiting_prompt: None,
            recv_ts: 10,
            claude_pid: None,
        };

        let snapshot = apply_notification_status(normalize(&event, &current), &current, &event);
        assert_eq!(snapshot.status, "processing");
    }

    #[test]
    fn materially_equal_ignores_seq_and_ts() {
        let a = Snapshot::empty(1);
        let mut b = Snapshot::empty(2);
        b.seq = 8;
        assert!(materially_equal(&a, &b));
    }

    fn make_event(name: &str) -> LocalHookEvent {
        LocalHookEvent {
            session_id: "sess".into(),
            cwd: "/tmp/project".into(),
            hook_event_name: name.into(),
            message: None,
            prompt_preview: None,
            tool_name: None,
            tool_use_id: None,
            permission_mode: "default".into(),
            waiting_prompt: None,
            recv_ts: 10,
            claude_pid: None,
        }
    }

    #[test]
    fn stop_maps_to_unknown_idle() {
        let current = Snapshot::empty(1);
        let snapshot = normalize(&make_event("Stop"), &current);
        assert_eq!(snapshot.status, "unknown");
    }

    #[test]
    fn subagent_stop_maps_to_unknown_idle() {
        let current = Snapshot::empty(1);
        let snapshot = normalize(&make_event("SubagentStop"), &current);
        assert_eq!(snapshot.status, "processing");
    }

    #[test]
    fn session_start_maps_to_unknown_idle() {
        let current = Snapshot::empty(1);
        let snapshot = normalize(&make_event("SessionStart"), &current);
        assert_eq!(snapshot.status, "unknown");
    }

    #[test]
    fn pre_tool_use_ask_user_question_maps_to_waiting() {
        let current = Snapshot::empty(1);
        let mut event = make_event("PreToolUse");
        event.tool_name = Some("AskUserQuestion".into());
        event.waiting_prompt = Some(WaitingPrompt {
            kind: WaitingPromptKind::AskUserQuestion,
            title: "Question".into(),
            question: "Pick one".into(),
            options: vec![WaitingPromptOption {
                label: "1. Execute".into(),
                description: Some("Run the plan now".into()),
            }],
        });
        let snapshot = normalize(&event, &current);
        assert_eq!(snapshot.status, "waiting_for_input");
        assert_eq!(snapshot.title, "Awaiting input");
        assert_eq!(snapshot.detail, "Pick one");
        assert!(snapshot.unread);
    }

    #[test]
    fn pre_tool_use_other_tool_maps_to_running_tool() {
        let current = Snapshot::empty(1);
        let mut event = make_event("PreToolUse");
        event.tool_name = Some("Write".into());
        let snapshot = normalize(&event, &current);
        assert_eq!(snapshot.status, "running_tool");
    }

    #[test]
    fn stop_failure_maps_to_waiting() {
        let current = Snapshot::empty(1);
        let snapshot = normalize(&make_event("StopFailure"), &current);
        assert_eq!(snapshot.status, "waiting_for_input");
    }

    #[test]
    fn materially_equal_detects_unread_change() {
        let mut a = Snapshot::empty(1);
        let mut b = Snapshot::empty(1);
        b.unread = true;
        assert!(!materially_equal(&a, &b));

        a.unread = true;
        assert!(materially_equal(&a, &b));
    }
}
