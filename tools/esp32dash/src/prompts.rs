use std::{
    collections::{HashMap, HashSet, VecDeque},
    sync::Arc,
    time::{Duration, Instant},
};

use serde::{Deserialize, Serialize};
use tokio::sync::{Mutex, Notify};

use crate::model::{LocalHookEvent, WaitingPrompt, WaitingPromptKind};

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DevicePrompt {
    pub id: String,
    pub transport_id: String,
    pub prompt: WaitingPrompt,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DismissedPrompt {
    pub id: String,
    pub transport_id: String,
    pub was_visible_on_device: bool,
}

/// One step of supersede-aware device sync: an older overlay to clear because a
/// newer prompt replaced it, and/or the overlay to show. The freshest live
/// pending prompt always wins the single device overlay.
#[derive(Debug, Default, PartialEq, Eq)]
pub struct PromptOverlayStep {
    pub dismiss: Option<DismissedPrompt>,
    pub show: Option<DevicePrompt>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PromptStatus {
    pub id: String,
    pub kind: String,
    pub title: String,
    pub question: String,
    pub resolved: bool,
    pub selection_index: Option<u8>,
    pub dismissed: bool,
    pub timed_out: bool,
}

#[derive(Debug)]
struct Entry {
    transport_id: String,
    prompt: WaitingPrompt,
    session_id: String,
    tool_use_id: Option<String>,
    resolution: Option<PromptResolution>,
    created: Instant,
    notify: Arc<Notify>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PromptResolution {
    pub selection_index: u8,
}

#[derive(Debug, Default)]
struct State {
    entries: HashMap<String, Entry>,
    pending_order: VecDeque<String>,
    device_visible: Option<String>,
}

#[derive(Debug, Clone)]
pub struct PromptStore {
    inner: Arc<Mutex<State>>,
    timeout: Duration,
}

impl PromptStore {
    #[allow(dead_code)]
    pub fn new() -> Self {
        Self {
            inner: Arc::new(Mutex::new(State::default())),
            timeout: Duration::from_secs(600),
        }
    }

    pub fn with_timeout(timeout: Duration) -> Self {
        Self {
            inner: Arc::new(Mutex::new(State::default())),
            timeout,
        }
    }

    pub async fn submit(
        &self,
        id: String,
        transport_id: String,
        prompt: WaitingPrompt,
        session_id: String,
        tool_use_id: Option<String>,
    ) -> String {
        let entry = Entry {
            transport_id,
            prompt,
            session_id,
            tool_use_id,
            resolution: None,
            created: Instant::now(),
            notify: Arc::new(Notify::new()),
        };
        let mut guard = self.inner.lock().await;
        remove_existing_locked(&mut guard, &id);
        guard.pending_order.push_back(id.clone());
        guard.entries.insert(id.clone(), entry);
        id
    }

    pub async fn dismiss_matching(&self, event: &LocalHookEvent) -> Vec<DismissedPrompt> {
        let mut guard = self.inner.lock().await;
        let visible_id = guard.device_visible.clone();
        let ids: Vec<String> = guard
            .entries
            .iter()
            .filter_map(|(id, entry)| {
                if is_entry_expired(entry, self.timeout) {
                    return None;
                }
                matches_event(entry, event).then_some(id.clone())
            })
            .collect();

        if ids.is_empty() {
            return Vec::new();
        }

        let remove_ids: HashSet<&str> = ids.iter().map(String::as_str).collect();
        let dismissed: Vec<DismissedPrompt> = ids
            .iter()
            .filter_map(|id| {
                let entry = guard.entries.get(id)?;
                Some(DismissedPrompt {
                    id: id.clone(),
                    transport_id: entry.transport_id.clone(),
                    was_visible_on_device: visible_id.as_deref() == Some(id.as_str()),
                })
            })
            .collect();

        for id in &ids {
            guard.entries.remove(id);
        }
        guard.pending_order.retain(|pending_id| !remove_ids.contains(pending_id.as_str()));
        if dismissed.iter().any(|prompt| prompt.was_visible_on_device) {
            guard.device_visible = None;
        }
        normalize_visible_locked(&mut guard, self.timeout);
        dismissed
    }

    /// Supersede-aware claim: the freshest live pending prompt wins the single
    /// device overlay. If a different (older) prompt is currently shown, it is
    /// returned in `dismiss` so the caller can clear it before showing `show`.
    pub async fn next_device_overlay(&self) -> PromptOverlayStep {
        let mut guard = self.inner.lock().await;
        retain_pending_order_locked(&mut guard, self.timeout);
        normalize_visible_locked(&mut guard, self.timeout);

        let Some(target_id) = guard.pending_order.back().cloned() else {
            return PromptOverlayStep::default();
        };
        if guard.device_visible.as_deref() == Some(target_id.as_str()) {
            return PromptOverlayStep::default();
        }

        // Clear whatever older overlay is currently shown.
        let mut dismiss = None;
        if let Some(old_id) = guard.device_visible.take() {
            if let Some(entry) = guard.entries.get(&old_id) {
                dismiss = Some(DismissedPrompt {
                    id: old_id.clone(),
                    transport_id: entry.transport_id.clone(),
                    was_visible_on_device: true,
                });
            }
        }

        // Show the freshest pending prompt.
        let show = guard.entries.get(&target_id).map(|entry| DevicePrompt {
            id: target_id.clone(),
            transport_id: entry.transport_id.clone(),
            prompt: entry.prompt.clone(),
        });
        if show.is_some() {
            guard.device_visible = Some(target_id);
        }

        PromptOverlayStep { dismiss, show }
    }

    pub async fn requeue_visible_for_device(&self) -> Option<DismissedPrompt> {
        let mut guard = self.inner.lock().await;
        let id = guard.device_visible.take()?;
        let entry = guard.entries.get(&id)?;
        Some(DismissedPrompt {
            id,
            transport_id: entry.transport_id.clone(),
            was_visible_on_device: true,
        })
    }

    pub async fn note_device_disconnected(&self) {
        self.inner.lock().await.device_visible = None;
    }

    pub async fn has_device_backlog(&self) -> bool {
        let mut guard = self.inner.lock().await;
        retain_pending_order_locked(&mut guard, self.timeout);
        normalize_visible_locked(&mut guard, self.timeout);
        guard.device_visible.is_some() || !guard.pending_order.is_empty()
    }

    pub async fn take_expired_visible_for_device(&self) -> Option<DismissedPrompt> {
        let mut guard = self.inner.lock().await;
        let expired_visible = guard.device_visible.as_ref().and_then(|id| {
            let entry = guard.entries.get(id)?;
            if entry.resolution.is_some() || !is_entry_expired(entry, self.timeout) {
                return None;
            }

            Some(DismissedPrompt {
                id: id.clone(),
                transport_id: entry.transport_id.clone(),
                was_visible_on_device: true,
            })
        });

        if expired_visible.is_some() {
            guard.device_visible = None;
        }

        retain_pending_order_locked(&mut guard, self.timeout);
        normalize_visible_locked(&mut guard, self.timeout);
        expired_visible
    }

    pub async fn status(&self, id: &str) -> Option<PromptStatus> {
        let guard = self.inner.lock().await;
        guard.entries.get(id).map(|entry| {
            let resolved = entry.resolution.is_some();
            let timed_out = entry.resolution.is_none() && is_entry_expired(entry, self.timeout);
            let selection_index = entry.resolution.map(|r| r.selection_index);
            PromptStatus {
                id: id.to_string(),
                kind: entry.prompt.kind.as_str().to_string(),
                title: entry.prompt.title.clone(),
                question: entry.prompt.question.clone(),
                resolved: resolved || timed_out,
                selection_index,
                dismissed: false,
                timed_out,
            }
        })
    }

    pub async fn cleanup_expired(&self) -> Vec<DismissedPrompt> {
        let mut guard = self.inner.lock().await;
        let visible_id = guard.device_visible.clone();
        let expired_ids: Vec<String> = guard
            .entries
            .iter()
            .filter_map(|(id, entry)| {
                if entry.resolution.is_none() && is_entry_expired(entry, self.timeout) {
                    Some(id.clone())
                } else {
                    None
                }
            })
            .collect();

        if expired_ids.is_empty() {
            return Vec::new();
        }

        let dismissed: Vec<DismissedPrompt> = expired_ids
            .iter()
            .filter_map(|id| {
                let entry = guard.entries.get(id)?;
                Some(DismissedPrompt {
                    id: id.clone(),
                    transport_id: entry.transport_id.clone(),
                    was_visible_on_device: visible_id.as_deref() == Some(id.as_str()),
                })
            })
            .collect();

        for id in &expired_ids {
            guard.entries.remove(id);
        }
        guard
            .pending_order
            .retain(|id| !expired_ids.contains(id));

        if dismissed.iter().any(|p| p.was_visible_on_device) {
            guard.device_visible = None;
        }

        dismissed
    }
}

fn is_entry_expired(entry: &Entry, timeout: Duration) -> bool {
    entry.created.elapsed() > timeout
}

fn retain_pending_order_locked(state: &mut State, timeout: Duration) {
    let entries = &state.entries;
    state.pending_order.retain(|id| {
        matches!(
            entries.get(id),
            Some(entry) if entry.resolution.is_none() && !is_entry_expired(entry, timeout)
        )
    });
}

fn normalize_visible_locked(state: &mut State, timeout: Duration) {
    let clear_visible = state.device_visible.as_ref().is_some_and(|id| {
        !matches!(
            state.entries.get(id),
            Some(entry) if entry.resolution.is_none() && !is_entry_expired(entry, timeout)
        )
    });

    if clear_visible {
        state.device_visible = None;
    }
}

fn remove_existing_locked(state: &mut State, id: &str) {
    state.entries.remove(id);
    state.pending_order.retain(|pending_id| pending_id != id);
    if state.device_visible.as_deref() == Some(id) {
        state.device_visible = None;
    }
}


fn matches_event(entry: &Entry, event: &LocalHookEvent) -> bool {
    if entry.session_id.is_empty() || event.session_id.is_empty() {
        return false;
    }
    if entry.session_id != event.session_id || event.hook_event_name == "Notification" {
        return false;
    }

    match entry.prompt.kind {
        WaitingPromptKind::AskUserQuestion
            if event.hook_event_name == "PreToolUse"
                && event.tool_name.as_deref() == Some("AskUserQuestion")
                && entry.tool_use_id == event.tool_use_id =>
        {
            event.waiting_prompt.as_ref() != Some(&entry.prompt)
        }
        WaitingPromptKind::Elicitation if event.hook_event_name == "Elicitation" => {
            event.waiting_prompt.as_ref() != Some(&entry.prompt)
        }
        _ => true,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::model::{WaitingPrompt, WaitingPromptOption};

    fn prompt(kind: WaitingPromptKind, title: &str) -> WaitingPrompt {
        WaitingPrompt {
            kind,
            title: title.into(),
            question: "Need input".into(),
            options: vec![WaitingPromptOption {
                label: "1. Continue".into(),
                description: Some("Use the terminal".into()),
            }],
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

    #[tokio::test]
    async fn claims_prompt_and_requeues_visible_prompt() {
        let store = PromptStore::new();
        store
            .submit(
                "prompt-1".into(),
                "prompt-transport-1".into(),
                prompt(WaitingPromptKind::Elicitation, "Awaiting input"),
                "sess-1".into(),
                None,
            )
            .await;

        let first = store.next_device_overlay().await.show.expect("pending should be shown");
        assert_eq!(first.id, "prompt-1");
        // Already showing it: idempotent no-op.
        let repeat = store.next_device_overlay().await;
        assert!(repeat.show.is_none() && repeat.dismiss.is_none());

        let dismissed = store.requeue_visible_for_device().await.unwrap();
        assert_eq!(
            dismissed,
            DismissedPrompt {
                id: "prompt-1".into(),
                transport_id: "prompt-transport-1".into(),
                was_visible_on_device: true,
            }
        );

        let retried = store.next_device_overlay().await.show.expect("pending should be shown again");
        assert_eq!(retried.id, "prompt-1");
    }

    #[tokio::test]
    async fn dismisses_matching_prompt_on_follow_up_event() {
        let store = PromptStore::new();
        store
            .submit(
                "prompt-1".into(),
                "prompt-transport-1".into(),
                prompt(WaitingPromptKind::AskUserQuestion, "Question"),
                "sess-1".into(),
                Some("tool-1".into()),
            )
            .await;

        let visible = store.next_device_overlay().await.show.expect("pending should be shown");
        assert_eq!(visible.id, "prompt-1");

        let dismissed = store
            .dismiss_matching(&event(
                "PostToolUse",
                "sess-1",
                Some("AskUserQuestion"),
                Some("tool-1"),
            ))
            .await;
        assert_eq!(
            dismissed,
            vec![DismissedPrompt {
                id: "prompt-1".into(),
                transport_id: "prompt-transport-1".into(),
                was_visible_on_device: true,
            }]
        );
        assert!(!store.has_device_backlog().await);
    }

    #[tokio::test]
    async fn duplicate_wait_event_does_not_clear_matching_prompt() {
        let store = PromptStore::new();
        store
            .submit(
                "prompt-1".into(),
                "prompt-transport-1".into(),
                prompt(WaitingPromptKind::AskUserQuestion, "Question"),
                "sess-1".into(),
                Some("tool-1".into()),
            )
            .await;

        let dismissed = store
            .dismiss_matching(&LocalHookEvent {
                waiting_prompt: Some(prompt(WaitingPromptKind::AskUserQuestion, "Question")),
                ..event("PreToolUse", "sess-1", Some("AskUserQuestion"), Some("tool-1"))
            })
            .await;
        assert!(dismissed.is_empty());
        assert!(store.has_device_backlog().await);
    }

    #[tokio::test]
    async fn prompt_times_out_and_is_cleaned_up() {
        let store = PromptStore::with_timeout(Duration::from_millis(50));
        store
            .submit(
                "prompt-1".into(),
                "prompt-transport-1".into(),
                prompt(WaitingPromptKind::Elicitation, "Awaiting input"),
                "sess-1".into(),
                None,
            )
            .await;

        // Show it on the device
        let claimed = store.next_device_overlay().await.show.expect("pending should be shown");
        assert_eq!(claimed.id, "prompt-1");

        // Wait for timeout
        tokio::time::sleep(Duration::from_millis(100)).await;

        // Should be able to claim again after cleanup
        let cleaned = store.take_expired_visible_for_device().await.unwrap();
        assert_eq!(cleaned.id, "prompt-1");
        assert!(cleaned.was_visible_on_device);

        // Backlog should be empty
        assert!(!store.has_device_backlog().await);
    }

    #[tokio::test]
    async fn cleanup_expired_removes_old_prompts() {
        let store = PromptStore::with_timeout(Duration::from_millis(50));
        store
            .submit(
                "prompt-1".into(),
                "prompt-transport-1".into(),
                prompt(WaitingPromptKind::Elicitation, "First"),
                "sess-1".into(),
                None,
            )
            .await;
        store
            .submit(
                "prompt-2".into(),
                "prompt-transport-2".into(),
                prompt(WaitingPromptKind::Elicitation, "Second"),
                "sess-1".into(),
                None,
            )
            .await;

        // Wait for timeout
        tokio::time::sleep(Duration::from_millis(100)).await;

        let dismissed = store.cleanup_expired().await;
        assert_eq!(dismissed.len(), 2);

        // Should not be in backlog anymore
        assert!(!store.has_device_backlog().await);
    }
}
