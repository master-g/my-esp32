use std::{
    collections::{HashMap, VecDeque},
    sync::Arc,
    time::{Duration, Instant},
};

use serde::{Deserialize, Serialize};
use tokio::sync::{Mutex, Notify};

use crate::model::LocalHookEvent;
use serde_json::Value;

#[cfg(test)]
const APPROVAL_TIMEOUT_DEFAULT: Duration = Duration::from_secs(300);

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ApprovalDecision {
    Allow,
    Deny,
    #[serde(alias = "yolo")]
    AllowAlways,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ApprovalRequest {
    pub tool_name: String,
    pub tool_input_summary: String,
    pub permission_mode: String,
    pub session_id: String,
    pub tool_use_id: Option<String>,
    #[serde(default)]
    pub permission_suggestions: Vec<Value>,
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
    #[serde(default)]
    pub permission_suggestions: Vec<Value>,
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

/// One step of supersede-aware device sync: an older overlay to clear because a
/// newer request replaced it, and/or the overlay to show. The freshest live
/// pending request always wins the single device overlay.
#[derive(Debug, Default, PartialEq, Eq)]
pub struct ApprovalOverlayStep {
    pub dismiss: Option<DismissedApproval>,
    pub show: Option<DeviceApproval>,
}

#[derive(Debug, Clone, Copy)]
enum ApprovalResolution {
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
    timeout: Duration,
}

impl ApprovalStore {
    #[allow(dead_code)]
    pub fn new() -> Self {
        Self {
            inner: Arc::new(Mutex::new(State::default())),
            timeout: Duration::from_secs(300),
        }
    }

    pub fn with_timeout(timeout: Duration) -> Self {
        Self {
            inner: Arc::new(Mutex::new(State::default())),
            timeout,
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

    pub async fn dismiss_matching(&self, event: &LocalHookEvent) -> Vec<DismissedApproval> {
        let mut guard = self.inner.lock().await;
        let mut dismissed = Vec::new();
        let visible_id = guard.device_visible.clone();

        for (id, entry) in guard.entries.iter_mut() {
            if entry.resolution.is_some() || is_entry_expired(entry, self.timeout) {
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
        retain_pending_order_locked(&mut guard, self.timeout);
        dismissed
    }

    /// Supersede-aware claim: the freshest live pending request wins the single
    /// device overlay. If a different (older) request is currently shown, it is
    /// returned in `dismiss` so the caller can clear it before showing `show`.
    pub async fn next_device_overlay(&self) -> ApprovalOverlayStep {
        let mut guard = self.inner.lock().await;
        retain_pending_order_locked(&mut guard, self.timeout);
        normalize_visible_locked(&mut guard, self.timeout);

        let Some(target_id) = guard.pending_order.back().cloned() else {
            return ApprovalOverlayStep::default();
        };
        if guard.device_visible.as_deref() == Some(target_id.as_str()) {
            return ApprovalOverlayStep::default();
        }

        // Clear whatever older overlay is currently shown.
        let mut dismiss = None;
        if let Some(old_id) = guard.device_visible.take() {
            if let Some(entry) = guard.entries.get(&old_id) {
                dismiss = Some(DismissedApproval {
                    id: old_id.clone(),
                    transport_id: entry.transport_id.clone(),
                    was_visible_on_device: true,
                });
            }
        }

        // Show the freshest pending request.
        let show = guard.entries.get(&target_id).map(|entry| DeviceApproval {
            id: target_id.clone(),
            transport_id: entry.transport_id.clone(),
            tool_name: entry.request.tool_name.clone(),
            tool_input_summary: entry.request.tool_input_summary.clone(),
        });
        if show.is_some() {
            guard.device_visible = Some(target_id);
        }

        ApprovalOverlayStep { dismiss, show }
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

    pub async fn take_expired_visible_for_device(&self) -> Option<DismissedApproval> {
        let mut guard = self.inner.lock().await;
        let expired_visible = guard.device_visible.as_ref().and_then(|id| {
            let entry = guard.entries.get(id)?;
            if entry.resolution.is_some() || !is_entry_expired(entry, self.timeout) {
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

        retain_pending_order_locked(&mut guard, self.timeout);
        normalize_visible_locked(&mut guard, self.timeout);
        expired_visible
    }

    /// Get the current status of an approval.
    pub async fn status(&self, id: &str) -> Option<ApprovalStatus> {
        let guard = self.inner.lock().await;
        guard.entries.get(id).map(|entry| {
            let timed_out = entry.resolution.is_none() && is_entry_expired(entry, self.timeout);
            let dismissed = matches!(entry.resolution, Some(ApprovalResolution::Dismissed));
            ApprovalStatus {
                id: id.to_string(),
                tool_name: entry.request.tool_name.clone(),
                tool_input_summary: entry.request.tool_input_summary.clone(),
                decision: None,
                resolved: entry.resolution.is_some() || timed_out,
                timed_out,
                dismissed,
                permission_suggestions: entry.request.permission_suggestions.clone(),
            }
        })
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
            permission_suggestions: Vec::new(),
        }
    }

    #[tokio::test]
    async fn dismisses_matching_pending_approvals() {
        let store = ApprovalStore::new();
        let other = store
            .submit("approve-2".into(), "approval-2".into(), request("sess-2", "tool-2", "Edit"))
            .await;
        let matching = store
            .submit("approve-1".into(), "approval-1".into(), request("sess-1", "tool-1", "Bash"))
            .await;

        // Freshest pending (matching) wins the device overlay.
        let step = store.next_device_overlay().await;
        let visible = step.show.expect("freshest pending should be shown");
        assert_eq!(visible.id, matching);
        assert!(step.dismiss.is_none());

        let dismissed = store
            .dismiss_matching(&LocalHookEvent {
                session_id: "sess-1".into(),
                cwd: "/tmp/project".into(),
                hook_event_name: "PreToolUse".into(),
                message: None,
                prompt_preview: None,
                prompt_raw: None,
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
    async fn supersedes_to_freshest_pending_approval() {
        let store = ApprovalStore::new();
        store
            .submit("approve-1".into(), "approval-1".into(), request("sess-1", "tool-1", "Bash"))
            .await;

        // First request shows with nothing to dismiss.
        let first_step = store.next_device_overlay().await;
        assert_eq!(
            first_step.show,
            Some(DeviceApproval {
                id: "approve-1".into(),
                transport_id: "approval-1".into(),
                tool_name: "Bash".into(),
                tool_input_summary: "Bash input".into(),
            })
        );
        assert!(first_step.dismiss.is_none());

        // A newer request supersedes the visible one.
        store
            .submit("approve-2".into(), "approval-2".into(), request("sess-1", "tool-2", "Edit"))
            .await;
        let second_step = store.next_device_overlay().await;
        assert_eq!(
            second_step.dismiss,
            Some(DismissedApproval {
                id: "approve-1".into(),
                transport_id: "approval-1".into(),
                was_visible_on_device: true,
            })
        );
        let shown = second_step.show.expect("freshest pending should be shown");
        assert_eq!(shown.id, "approve-2");
        assert_eq!(shown.transport_id, "approval-2");

        // Already showing the freshest: idempotent no-op.
        let third_step = store.next_device_overlay().await;
        assert!(third_step.show.is_none());
        assert!(third_step.dismiss.is_none());
    }

    #[tokio::test]
    async fn disconnection_requeues_visible_approval() {
        let store = ApprovalStore::new();
        store
            .submit("approve-1".into(), "approval-1".into(), request("sess-1", "tool-1", "Bash"))
            .await;

        let first = store.next_device_overlay().await.show.expect("pending should be shown");
        assert_eq!(first.id, "approve-1");

        // Already showing it: repeat is a no-op.
        let repeat = store.next_device_overlay().await;
        assert!(repeat.show.is_none() && repeat.dismiss.is_none());

        store.note_device_disconnected().await;

        let retried = store.next_device_overlay().await.show.expect("pending should be shown again");
        assert_eq!(retried.id, "approve-1");
        assert_eq!(retried.transport_id, "approval-1");
    }

    #[tokio::test]
    async fn visible_timeout_requests_device_dismissal_and_advances_queue() {
        let store = ApprovalStore::new();
        store
            .submit("approve-2".into(), "approval-2".into(), request("sess-1", "tool-2", "Edit"))
            .await;
        store
            .submit("approve-1".into(), "approval-1".into(), request("sess-1", "tool-1", "Bash"))
            .await;

        // Freshest pending (approve-1) is the one shown on the device.
        let first = store.next_device_overlay().await.show.expect("freshest pending should be shown");
        assert_eq!(first.id, "approve-1");

        {
            let mut guard = store.inner.lock().await;
            let entry = guard.entries.get_mut("approve-1").unwrap();
            entry.created = Instant::now() - APPROVAL_TIMEOUT_DEFAULT - Duration::from_secs(1);
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

        // The next-freshest remaining pending advances onto the device.
        let next = store.next_device_overlay().await.show.expect("next pending should be shown");
        assert_eq!(next.id, "approve-2");

        let expired_status = store.status("approve-1").await.unwrap();
        assert!(expired_status.resolved);
        assert!(expired_status.timed_out);
        assert!(expired_status.decision.is_none());
    }
}
