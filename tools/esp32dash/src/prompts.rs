use std::{
    collections::{HashMap, HashSet, VecDeque},
    sync::Arc,
};

use tokio::sync::Mutex;

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

#[derive(Debug)]
struct Entry {
    transport_id: String,
    prompt: WaitingPrompt,
    session_id: String,
    tool_use_id: Option<String>,
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
}

impl PromptStore {
    pub fn new() -> Self {
        Self {
            inner: Arc::new(Mutex::new(State::default())),
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
            .filter_map(|(id, entry)| matches_event(entry, event).then_some(id.clone()))
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
        normalize_visible_locked(&mut guard);
        dismissed
    }

    pub async fn claim_next_for_device(&self) -> Option<DevicePrompt> {
        let mut guard = self.inner.lock().await;
        retain_pending_order_locked(&mut guard);
        normalize_visible_locked(&mut guard);

        if guard.device_visible.is_some() {
            return None;
        }

        let id = guard.pending_order.front()?.clone();
        let prompt = {
            let entry = guard.entries.get(&id)?;
            DevicePrompt {
                id: id.clone(),
                transport_id: entry.transport_id.clone(),
                prompt: entry.prompt.clone(),
            }
        };

        guard.device_visible = Some(id);
        Some(prompt)
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
        retain_pending_order_locked(&mut guard);
        normalize_visible_locked(&mut guard);
        guard.device_visible.is_some() || !guard.pending_order.is_empty()
    }
}

fn retain_pending_order_locked(state: &mut State) {
    let entries = &state.entries;
    state.pending_order.retain(|id| entries.contains_key(id));
}

fn normalize_visible_locked(state: &mut State) {
    let clear_visible =
        state.device_visible.as_ref().is_some_and(|id| !state.entries.contains_key(id));
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

        let first = store.claim_next_for_device().await.unwrap();
        assert_eq!(first.id, "prompt-1");
        assert!(store.claim_next_for_device().await.is_none());

        let dismissed = store.requeue_visible_for_device().await.unwrap();
        assert_eq!(
            dismissed,
            DismissedPrompt {
                id: "prompt-1".into(),
                transport_id: "prompt-transport-1".into(),
                was_visible_on_device: true,
            }
        );

        let retried = store.claim_next_for_device().await.unwrap();
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

        let visible = store.claim_next_for_device().await.unwrap();
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
}
