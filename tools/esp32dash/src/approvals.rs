use std::{
    collections::{HashMap, VecDeque},
    sync::Arc,
    time::{Duration, Instant},
};

use serde::{Deserialize, Serialize};
use tokio::sync::{Mutex, Notify};

use crate::model::LocalHookEvent;

const APPROVAL_TIMEOUT: Duration = Duration::from_secs(300);

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ApprovalDecision {
    Allow,
    Deny,
    Yolo,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ApprovalRequest {
    pub tool_name: String,
    pub tool_input_summary: String,
    pub permission_mode: String,
    pub session_id: String,
    pub tool_use_id: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ApprovalStatus {
    pub id: String,
    pub tool_name: String,
    pub tool_input_summary: String,
    pub decision: Option<ApprovalDecision>,
    pub resolved: bool,
    pub timed_out: bool,
    pub dismissed: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DeviceApproval {
    pub id: String,
    pub transport_id: String,
    pub tool_name: String,
    pub tool_input_summary: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DismissedApproval {
    pub id: String,
    pub transport_id: String,
    pub was_visible_on_device: bool,
}

#[derive(Debug, Clone, Copy)]
enum ApprovalResolution {
    Decision(ApprovalDecision),
    Dismissed,
}

#[derive(Debug)]
struct Entry {
    transport_id: String,
    request: ApprovalRequest,
    resolution: Option<ApprovalResolution>,
    created: Instant,
    notify: Arc<Notify>,
}

#[derive(Debug, Default)]
struct State {
    entries: HashMap<String, Entry>,
    transport_index: HashMap<String, String>,
    pending_order: VecDeque<String>,
    device_visible: Option<String>,
}

#[derive(Debug, Clone)]
pub struct ApprovalStore {
    inner: Arc<Mutex<State>>,
}

impl ApprovalStore {
    pub fn new() -> Self {
        Self {
            inner: Arc::new(Mutex::new(State::default())),
        }
    }

    /// Submit a new approval request. Returns a unique ID.
    pub async fn submit(
        &self,
        id: String,
        transport_id: String,
        request: ApprovalRequest,
    ) -> String {
        let entry = Entry {
            transport_id: transport_id.clone(),
            request,
            resolution: None,
            created: Instant::now(),
            notify: Arc::new(Notify::new()),
        };
        let mut guard = self.inner.lock().await;
        remove_existing_locked(&mut guard, &id);
        guard.transport_index.insert(transport_id, id.clone());
        guard.pending_order.push_back(id.clone());
        guard.entries.insert(id.clone(), entry);
        id
    }

    /// Resolve a pending approval sent to the device. Returns the approval id if found.
    pub async fn resolve(&self, transport_id: &str, decision: ApprovalDecision) -> Option<String> {
        let mut guard = self.inner.lock().await;
        let id = guard.transport_index.get(transport_id)?.clone();

        {
            let entry = guard.entries.get_mut(&id)?;
            if entry.resolution.is_some() || is_entry_expired(entry) {
                return None;
            }
            entry.resolution = Some(ApprovalResolution::Decision(decision));
            entry.notify.notify_waiters();
        }

        guard.pending_order.retain(|pending_id| pending_id != &id);
        if guard.device_visible.as_deref() == Some(id.as_str()) {
            guard.device_visible = None;
        }

        Some(id)
    }

    pub async fn dismiss_matching(&self, event: &LocalHookEvent) -> Vec<DismissedApproval> {
        let mut guard = self.inner.lock().await;
        let mut dismissed = Vec::new();
        let visible_id = guard.device_visible.clone();

        for (id, entry) in guard.entries.iter_mut() {
            if entry.resolution.is_some() || is_entry_expired(entry) {
                continue;
            }
            if !matches_event(entry, event) {
                continue;
            }

            entry.resolution = Some(ApprovalResolution::Dismissed);
            entry.notify.notify_waiters();
            dismissed.push(DismissedApproval {
                id: id.clone(),
                transport_id: entry.transport_id.clone(),
                was_visible_on_device: visible_id.as_deref() == Some(id.as_str()),
            });
        }

        if dismissed.iter().any(|approval| approval.was_visible_on_device) {
            guard.device_visible = None;
        }
        retain_pending_order_locked(&mut guard);
        dismissed
    }

    pub async fn claim_next_for_device(&self) -> Option<DeviceApproval> {
        let mut guard = self.inner.lock().await;
        retain_pending_order_locked(&mut guard);
        normalize_visible_locked(&mut guard);

        if guard.device_visible.is_some() {
            return None;
        }

        let id = guard.pending_order.front()?.clone();
        let approval = {
            let entry = guard.entries.get(&id)?;
            if entry.resolution.is_some() || is_entry_expired(entry) {
                return None;
            }

            DeviceApproval {
                id: id.clone(),
                transport_id: entry.transport_id.clone(),
                tool_name: entry.request.tool_name.clone(),
                tool_input_summary: entry.request.tool_input_summary.clone(),
            }
        };

        guard.device_visible = Some(id);
        Some(approval)
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

    pub async fn take_expired_visible_for_device(&self) -> Option<DismissedApproval> {
        let mut guard = self.inner.lock().await;
        let expired_visible = guard.device_visible.as_ref().and_then(|id| {
            let entry = guard.entries.get(id)?;
            if entry.resolution.is_some() || !is_entry_expired(entry) {
                return None;
            }

            Some(DismissedApproval {
                id: id.clone(),
                transport_id: entry.transport_id.clone(),
                was_visible_on_device: true,
            })
        });

        if expired_visible.is_some() {
            guard.device_visible = None;
        }

        retain_pending_order_locked(&mut guard);
        normalize_visible_locked(&mut guard);
        expired_visible
    }

    /// Get the current status of an approval.
    pub async fn status(&self, id: &str) -> Option<ApprovalStatus> {
        let guard = self.inner.lock().await;
        guard.entries.get(id).map(|entry| {
            let timed_out = entry.resolution.is_none() && is_entry_expired(entry);
            let decision = match entry.resolution {
                Some(ApprovalResolution::Decision(decision)) => Some(decision),
                _ => None,
            };
            let dismissed = matches!(entry.resolution, Some(ApprovalResolution::Dismissed));
            ApprovalStatus {
                id: id.to_string(),
                tool_name: entry.request.tool_name.clone(),
                tool_input_summary: entry.request.tool_input_summary.clone(),
                decision,
                resolved: entry.resolution.is_some() || timed_out,
                timed_out,
                dismissed,
            }
        })
    }

    /// Wait until the approval is resolved or times out. Returns the decision.
    pub async fn wait_for_decision(
        &self,
        id: &str,
        poll_interval: Duration,
    ) -> Option<ApprovalDecision> {
        loop {
            let notify = {
                let guard = self.inner.lock().await;
                let entry = guard.entries.get(id)?;
                match entry.resolution {
                    Some(ApprovalResolution::Decision(decision)) => return Some(decision),
                    Some(ApprovalResolution::Dismissed) => return None,
                    None => {}
                }
                if is_entry_expired(entry) {
                    return None; // timed out
                }
                entry.notify.clone()
            };

            tokio::select! {
                _ = notify.notified() => {}
                _ = tokio::time::sleep(poll_interval) => {}
            }
        }
    }
}

fn is_entry_expired(entry: &Entry) -> bool {
    entry.created.elapsed() > APPROVAL_TIMEOUT
}

fn retain_pending_order_locked(state: &mut State) {
    let entries = &state.entries;
    state.pending_order.retain(|id| {
        matches!(
            entries.get(id),
            Some(entry) if entry.resolution.is_none() && !is_entry_expired(entry)
        )
    });
}

fn normalize_visible_locked(state: &mut State) {
    let clear_visible = state.device_visible.as_ref().is_some_and(|id| {
        !matches!(
            state.entries.get(id),
            Some(entry) if entry.resolution.is_none() && !is_entry_expired(entry)
        )
    });

    if clear_visible {
        state.device_visible = None;
    }
}

fn remove_existing_locked(state: &mut State, id: &str) {
    if let Some(entry) = state.entries.remove(id) {
        state.transport_index.remove(&entry.transport_id);
    }
    state.pending_order.retain(|pending_id| pending_id != id);
    if state.device_visible.as_deref() == Some(id) {
        state.device_visible = None;
    }
}

fn matches_event(entry: &Entry, event: &LocalHookEvent) -> bool {
    if entry.request.session_id.is_empty() || event.session_id.is_empty() {
        return false;
    }
    if entry.request.session_id != event.session_id {
        return false;
    }

    match (entry.request.tool_use_id.as_deref(), event.tool_use_id.as_deref()) {
        (Some(expected), Some(actual)) => expected == actual,
        _ => true,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn request(session_id: &str, tool_use_id: &str, tool_name: &str) -> ApprovalRequest {
        ApprovalRequest {
            tool_name: tool_name.into(),
            tool_input_summary: format!("{tool_name} input"),
            permission_mode: "default".into(),
            session_id: session_id.into(),
            tool_use_id: Some(tool_use_id.into()),
        }
    }

    #[tokio::test]
    async fn submit_and_resolve() {
        let store = ApprovalStore::new();
        let id = store
            .submit("test-1".into(), "transport-1".into(), request("sess-1", "tool-1", "Bash"))
            .await;

        let status = store.status(&id).await.unwrap();
        assert!(!status.resolved);
        assert!(status.decision.is_none());
        assert!(!status.dismissed);

        assert_eq!(store.resolve("transport-1", ApprovalDecision::Allow).await, Some(id.clone()));

        let status = store.status(&id).await.unwrap();
        assert!(status.resolved);
        assert_eq!(status.decision, Some(ApprovalDecision::Allow));
        assert!(!status.dismissed);
    }

    #[tokio::test]
    async fn wait_resolves_on_notify() {
        let store = ApprovalStore::new();
        let id = store
            .submit("test-2".into(), "transport-2".into(), request("sess-1", "tool-2", "Edit"))
            .await;

        let store2 = store.clone();
        tokio::spawn(async move {
            tokio::time::sleep(Duration::from_millis(50)).await;
            store2.resolve("transport-2", ApprovalDecision::Deny).await;
        });

        let decision = store.wait_for_decision(&id, Duration::from_millis(10)).await;
        assert_eq!(decision, Some(ApprovalDecision::Deny));
    }

    #[tokio::test]
    async fn dismisses_matching_pending_approvals() {
        let store = ApprovalStore::new();
        let matching = store
            .submit("approve-1".into(), "approval-1".into(), request("sess-1", "tool-1", "Bash"))
            .await;
        let other = store
            .submit("approve-2".into(), "approval-2".into(), request("sess-2", "tool-2", "Edit"))
            .await;

        let visible = store.claim_next_for_device().await.unwrap();
        assert_eq!(visible.id, matching);

        let dismissed = store
            .dismiss_matching(&LocalHookEvent {
                session_id: "sess-1".into(),
                cwd: "/tmp/project".into(),
                hook_event_name: "PreToolUse".into(),
                message: None,
                prompt_preview: None,
                tool_name: Some("Bash".into()),
                tool_use_id: Some("tool-1".into()),
                permission_mode: "default".into(),
                waiting_prompt: None,
                recv_ts: 1,
                claude_pid: None,
            })
            .await;

        assert_eq!(
            dismissed,
            vec![DismissedApproval {
                id: matching.clone(),
                transport_id: "approval-1".into(),
                was_visible_on_device: true,
            }]
        );

        let matching_status = store.status(&matching).await.unwrap();
        assert!(matching_status.resolved);
        assert!(matching_status.dismissed);
        assert!(matching_status.decision.is_none());

        let other_status = store.status(&other).await.unwrap();
        assert!(!other_status.resolved);
        assert!(!other_status.dismissed);
    }

    #[tokio::test]
    async fn claims_device_approvals_in_fifo_order() {
        let store = ApprovalStore::new();
        store
            .submit("approve-1".into(), "approval-1".into(), request("sess-1", "tool-1", "Bash"))
            .await;
        store
            .submit("approve-2".into(), "approval-2".into(), request("sess-1", "tool-2", "Edit"))
            .await;

        let first = store.claim_next_for_device().await.unwrap();
        assert_eq!(
            first,
            DeviceApproval {
                id: "approve-1".into(),
                transport_id: "approval-1".into(),
                tool_name: "Bash".into(),
                tool_input_summary: "Bash input".into(),
            }
        );
        assert!(store.claim_next_for_device().await.is_none());

        assert_eq!(
            store.resolve("approval-1", ApprovalDecision::Allow).await,
            Some("approve-1".into())
        );

        let second = store.claim_next_for_device().await.unwrap();
        assert_eq!(second.id, "approve-2");
        assert_eq!(second.transport_id, "approval-2");
    }

    #[tokio::test]
    async fn disconnection_requeues_visible_approval() {
        let store = ApprovalStore::new();
        store
            .submit("approve-1".into(), "approval-1".into(), request("sess-1", "tool-1", "Bash"))
            .await;

        let first = store.claim_next_for_device().await.unwrap();
        assert_eq!(first.id, "approve-1");

        store.note_device_disconnected().await;

        let retried = store.claim_next_for_device().await.unwrap();
        assert_eq!(retried.id, "approve-1");
        assert_eq!(retried.transport_id, "approval-1");
    }

    #[tokio::test]
    async fn visible_timeout_requests_device_dismissal_and_advances_queue() {
        let store = ApprovalStore::new();
        store
            .submit("approve-1".into(), "approval-1".into(), request("sess-1", "tool-1", "Bash"))
            .await;
        store
            .submit("approve-2".into(), "approval-2".into(), request("sess-1", "tool-2", "Edit"))
            .await;

        let first = store.claim_next_for_device().await.unwrap();
        assert_eq!(first.id, "approve-1");

        {
            let mut guard = store.inner.lock().await;
            let entry = guard.entries.get_mut("approve-1").unwrap();
            entry.created = Instant::now() - APPROVAL_TIMEOUT - Duration::from_secs(1);
        }

        let expired = store.take_expired_visible_for_device().await.unwrap();
        assert_eq!(
            expired,
            DismissedApproval {
                id: "approve-1".into(),
                transport_id: "approval-1".into(),
                was_visible_on_device: true,
            }
        );

        let next = store.claim_next_for_device().await.unwrap();
        assert_eq!(next.id, "approve-2");

        let expired_status = store.status("approve-1").await.unwrap();
        assert!(expired_status.resolved);
        assert!(expired_status.timed_out);
        assert!(expired_status.decision.is_none());
    }
}
