//! Pending interaction queue: every Claude interaction waiting for the user on the
//! Mac (see GLOSSARY.md). The queue answers one question — which interaction the
//! single device slot should show right now — and owns the rules behind it:
//! an approval beats a prompt, the freshest of a kind wins, entries time out per
//! kind, and follow-up hook events from the same session clear them.

use std::time::{Duration, Instant};

use crate::{
    config::ApprovalConfig,
    model::{LocalHookEvent, PendingDisplay, WaitingPrompt, WaitingPromptKind},
};

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Interaction {
    Approval {
        tool_name: String,
    },
    Prompt(WaitingPrompt),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PendingInteraction {
    pub id: String,
    pub session_id: String,
    pub tool_use_id: Option<String>,
    pub interaction: Interaction,
}

impl PendingInteraction {
    pub fn is_approval(&self) -> bool {
        matches!(self.interaction, Interaction::Approval { .. })
    }

    pub fn display(&self) -> PendingDisplay {
        PendingDisplay {
            kind: if self.is_approval() {
                "approval"
            } else {
                "prompt"
            }
            .into(),
            coarse_type: self.coarse_type().into(),
        }
    }

    /// Coarse type shown on the read-only device. Never tool arguments, question or
    /// option text (R2); unknown tools collapse to a generic label so arbitrary
    /// (e.g. MCP) tool names don't leak to the always-on screen.
    pub fn coarse_type(&self) -> &'static str {
        match &self.interaction {
            Interaction::Approval {
                tool_name,
            } => match tool_name.as_str() {
                "Bash" => "Bash command",
                "Edit" | "Write" | "MultiEdit" | "NotebookEdit" => "File edit",
                "Read" | "NotebookRead" => "File read",
                "Glob" | "Grep" | "LS" => "File search",
                "WebFetch" | "WebSearch" => "Web request",
                "Task" => "Subagent task",
                _ => "Tool request",
            },
            Interaction::Prompt(prompt) => match prompt.kind {
                WaitingPromptKind::Elicitation => "Input request",
                WaitingPromptKind::AskUserQuestion => "Question",
            },
        }
    }
}

#[derive(Debug, Clone, Copy)]
pub struct PendingTimeouts {
    pub approval: Duration,
    pub elicitation: Duration,
    pub question: Duration,
}

impl From<&ApprovalConfig> for PendingTimeouts {
    fn from(config: &ApprovalConfig) -> Self {
        Self {
            approval: Duration::from_secs(config.timeout_secs),
            elicitation: Duration::from_secs(config.elicitation_timeout_secs),
            question: Duration::from_secs(config.question_timeout_secs),
        }
    }
}

#[derive(Debug)]
struct Entry {
    created: Instant,
    item: PendingInteraction,
}

#[derive(Debug)]
pub struct PendingQueue {
    timeouts: PendingTimeouts,
    /// Oldest first.
    entries: Vec<Entry>,
}

impl PendingQueue {
    pub fn new(timeouts: PendingTimeouts) -> Self {
        Self {
            timeouts,
            entries: Vec::new(),
        }
    }

    /// Queue an interaction as the freshest one. Resubmitting an id replaces it.
    pub fn submit(&mut self, item: PendingInteraction, now: Instant) {
        self.entries.retain(|entry| entry.item.id != item.id);
        self.entries.push(Entry {
            created: now,
            item,
        });
    }

    /// Drop every interaction this hook event resolves; returns their ids.
    pub fn clear_for_event(&mut self, event: &LocalHookEvent) -> Vec<String> {
        let mut cleared = Vec::new();
        self.entries.retain(|entry| {
            let clears = event_clears(&entry.item, event);
            if clears {
                cleared.push(entry.item.id.clone());
            }
            !clears
        });
        cleared
    }

    /// The interaction the device should show: the freshest approval, else the
    /// freshest prompt.
    pub fn current(&mut self, now: Instant) -> Option<&PendingInteraction> {
        self.drop_expired(now);
        let items = || self.entries.iter().rev().map(|entry| &entry.item);
        items().find(|item| item.is_approval()).or_else(|| items().next())
    }

    fn drop_expired(&mut self, now: Instant) {
        let timeouts = self.timeouts;
        self.entries.retain(|entry| {
            let timeout = match &entry.item.interaction {
                Interaction::Approval {
                    ..
                } => timeouts.approval,
                Interaction::Prompt(prompt) => match prompt.kind {
                    WaitingPromptKind::Elicitation => timeouts.elicitation,
                    WaitingPromptKind::AskUserQuestion => timeouts.question,
                },
            };
            now.saturating_duration_since(entry.created) <= timeout
        });
    }
}

fn event_clears(item: &PendingInteraction, event: &LocalHookEvent) -> bool {
    if item.session_id.is_empty()
        || item.session_id != event.session_id
        || event.hook_event_name == "Notification"
    {
        return false;
    }

    match &item.interaction {
        // Further permission requests from the session stack up; anything else
        // means the session moved on, unless it names a different tool call.
        Interaction::Approval {
            ..
        } => {
            event.hook_event_name != "PermissionRequest"
                && match (item.tool_use_id.as_deref(), event.tool_use_id.as_deref()) {
                    (Some(expected), Some(actual)) => expected == actual,
                    _ => true,
                }
        }
        // The same wait event arriving again must not clear its own prompt.
        Interaction::Prompt(prompt) => match prompt.kind {
            WaitingPromptKind::AskUserQuestion
                if event.hook_event_name == "PreToolUse"
                    && event.tool_name.as_deref() == Some("AskUserQuestion")
                    && item.tool_use_id == event.tool_use_id =>
            {
                event.waiting_prompt.as_ref() != Some(prompt)
            }
            WaitingPromptKind::Elicitation if event.hook_event_name == "Elicitation" => {
                event.waiting_prompt.as_ref() != Some(prompt)
            }
            _ => true,
        },
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::model::WaitingPromptOption;

    fn timeouts() -> PendingTimeouts {
        (&ApprovalConfig::default()).into()
    }

    fn approval(
        id: &str,
        session_id: &str,
        tool_use_id: &str,
        tool_name: &str,
    ) -> PendingInteraction {
        PendingInteraction {
            id: id.into(),
            session_id: session_id.into(),
            tool_use_id: Some(tool_use_id.into()),
            interaction: Interaction::Approval {
                tool_name: tool_name.into(),
            },
        }
    }

    fn waiting_prompt(kind: WaitingPromptKind) -> WaitingPrompt {
        WaitingPrompt {
            kind,
            title: "Awaiting input".into(),
            question: "Need input".into(),
            options: vec![WaitingPromptOption {
                label: "1. Continue".into(),
                description: Some("Use the terminal".into()),
            }],
        }
    }

    fn prompt(
        id: &str,
        session_id: &str,
        tool_use_id: Option<&str>,
        kind: WaitingPromptKind,
    ) -> PendingInteraction {
        PendingInteraction {
            id: id.into(),
            session_id: session_id.into(),
            tool_use_id: tool_use_id.map(str::to_string),
            interaction: Interaction::Prompt(waiting_prompt(kind)),
        }
    }

    fn event(
        name: &str,
        session_id: &str,
        tool_name: Option<&str>,
        tool_use_id: Option<&str>,
    ) -> LocalHookEvent {
        LocalHookEvent {
            session_id: session_id.into(),
            cwd: "/tmp/project".into(),
            hook_event_name: name.into(),
            message: None,
            prompt_preview: None,
            prompt_raw: None,
            tool_name: tool_name.map(str::to_string),
            tool_use_id: tool_use_id.map(str::to_string),
            permission_mode: "default".into(),
            waiting_prompt: None,
            recv_ts: 1,
            claude_pid: None,
        }
    }

    fn current_id(queue: &mut PendingQueue, now: Instant) -> Option<String> {
        queue.current(now).map(|item| item.id.clone())
    }

    #[test]
    fn coarse_types_map_to_fixed_labels_without_leaking_content() {
        let coarse = |tool: &str| approval("a", "s", "t", tool).coarse_type();
        assert_eq!(coarse("Bash"), "Bash command");
        assert_eq!(coarse("Write"), "File edit");
        assert_eq!(coarse("Read"), "File read");
        assert_eq!(coarse("mcp__github__create_issue"), "Tool request");
        assert_eq!(
            prompt("p", "s", None, WaitingPromptKind::AskUserQuestion).coarse_type(),
            "Question"
        );
        assert_eq!(
            prompt("p", "s", None, WaitingPromptKind::Elicitation).coarse_type(),
            "Input request"
        );
    }

    #[test]
    fn freshest_of_a_kind_is_current_and_older_returns_when_it_clears() {
        let now = Instant::now();
        let mut queue = PendingQueue::new(timeouts());
        queue.submit(approval("approve-a", "sess-a", "tool-a", "Bash"), now);
        assert_eq!(current_id(&mut queue, now).as_deref(), Some("approve-a"));

        queue.submit(approval("approve-b", "sess-b", "tool-b", "Edit"), now);
        assert_eq!(current_id(&mut queue, now).as_deref(), Some("approve-b"));

        let cleared = queue.clear_for_event(&event("PostToolUse", "sess-b", None, Some("tool-b")));
        assert_eq!(cleared, vec!["approve-b".to_string()]);
        assert_eq!(current_id(&mut queue, now).as_deref(), Some("approve-a"));
    }

    #[test]
    fn approval_preempts_prompt_and_prompt_returns_afterwards() {
        let now = Instant::now();
        let mut queue = PendingQueue::new(timeouts());
        queue.submit(prompt("prompt-1", "sess-1", None, WaitingPromptKind::Elicitation), now);
        assert_eq!(current_id(&mut queue, now).as_deref(), Some("prompt-1"));

        // An older approval still beats a newer prompt.
        queue.submit(approval("approve-1", "sess-2", "tool-1", "Bash"), now);
        queue.submit(prompt("prompt-2", "sess-3", None, WaitingPromptKind::Elicitation), now);
        assert_eq!(current_id(&mut queue, now).as_deref(), Some("approve-1"));

        queue.clear_for_event(&event("PostToolUse", "sess-2", None, Some("tool-1")));
        assert_eq!(current_id(&mut queue, now).as_deref(), Some("prompt-2"));
    }

    #[test]
    fn approval_clears_only_for_its_session_and_tool_call() {
        let now = Instant::now();
        let mut queue = PendingQueue::new(timeouts());
        queue.submit(approval("approve-1", "sess-1", "tool-1", "Bash"), now);

        for unrelated in [
            event("PostToolUse", "sess-2", None, Some("tool-1")),
            event("PostToolUse", "sess-1", None, Some("tool-other")),
            event("PermissionRequest", "sess-1", None, None),
            event("Notification", "sess-1", None, None),
            event("Stop", "", None, None),
        ] {
            assert!(queue.clear_for_event(&unrelated).is_empty(), "{unrelated:?}");
        }

        assert_eq!(
            queue.clear_for_event(&event("Stop", "sess-1", None, None)),
            vec!["approve-1".to_string()]
        );
        assert!(queue.current(now).is_none());
    }

    #[test]
    fn prompt_clears_on_follow_up_event() {
        let now = Instant::now();
        let mut queue = PendingQueue::new(timeouts());
        queue.submit(
            prompt("prompt-1", "sess-1", Some("tool-1"), WaitingPromptKind::AskUserQuestion),
            now,
        );

        let cleared = queue.clear_for_event(&event(
            "PostToolUse",
            "sess-1",
            Some("AskUserQuestion"),
            Some("tool-1"),
        ));
        assert_eq!(cleared, vec!["prompt-1".to_string()]);
        assert!(queue.current(now).is_none());
    }

    #[test]
    fn duplicate_wait_event_does_not_clear_matching_prompt() {
        let now = Instant::now();
        let mut queue = PendingQueue::new(timeouts());
        queue.submit(
            prompt("prompt-1", "sess-1", Some("tool-1"), WaitingPromptKind::AskUserQuestion),
            now,
        );

        let cleared = queue.clear_for_event(&LocalHookEvent {
            waiting_prompt: Some(waiting_prompt(WaitingPromptKind::AskUserQuestion)),
            ..event("PreToolUse", "sess-1", Some("AskUserQuestion"), Some("tool-1"))
        });
        assert!(cleared.is_empty());
        assert!(queue.current(now).is_some());
    }

    #[test]
    fn entries_time_out_per_kind_and_the_queue_advances() {
        let now = Instant::now();
        let limits = timeouts();
        assert!(limits.elicitation < limits.question && limits.question < limits.approval);

        let mut queue = PendingQueue::new(limits);
        queue.submit(prompt("elicit", "sess-1", None, WaitingPromptKind::Elicitation), now);
        queue.submit(prompt("ask", "sess-2", None, WaitingPromptKind::AskUserQuestion), now);
        queue.submit(approval("approve", "sess-3", "tool-1", "Bash"), now);

        // Exactly at the limit an entry is still live.
        assert_eq!(current_id(&mut queue, now + limits.approval).as_deref(), Some("approve"));

        let mut queue = PendingQueue::new(limits);
        queue.submit(prompt("elicit", "sess-1", None, WaitingPromptKind::Elicitation), now);
        queue.submit(prompt("ask", "sess-2", None, WaitingPromptKind::AskUserQuestion), now);
        let after_elicitation = now + limits.elicitation + Duration::from_secs(1);
        assert_eq!(current_id(&mut queue, after_elicitation).as_deref(), Some("ask"));
        let after_question = now + limits.question + Duration::from_secs(1);
        assert!(queue.current(after_question).is_none());
    }

    #[test]
    fn resubmitting_an_id_replaces_it_as_freshest() {
        let now = Instant::now();
        let mut queue = PendingQueue::new(timeouts());
        queue.submit(approval("approve-1", "sess-1", "tool-1", "Bash"), now);
        queue.submit(approval("approve-2", "sess-2", "tool-2", "Edit"), now);
        queue.submit(approval("approve-1", "sess-1", "tool-1", "Bash"), now);

        assert_eq!(current_id(&mut queue, now).as_deref(), Some("approve-1"));
        queue.clear_for_event(&event("Stop", "sess-1", None, None));
        assert_eq!(current_id(&mut queue, now).as_deref(), Some("approve-2"));
        queue.clear_for_event(&event("Stop", "sess-2", None, None));
        assert!(queue.current(now).is_none());
    }
}
