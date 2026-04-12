use std::{
    collections::HashMap,
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

#[derive(Debug, Clone, Copy)]
enum ApprovalResolution {
    Decision(ApprovalDecision),
    Dismissed,
}

#[derive(Debug)]
struct Entry {
    request: ApprovalRequest,
    resolution: Option<ApprovalResolution>,
    created: Instant,
    notify: Arc<Notify>,
}

#[derive(Debug, Clone)]
pub struct ApprovalStore {
    inner: Arc<Mutex<HashMap<String, Entry>>>,
}

impl ApprovalStore {
    pub fn new() -> Self {
        Self {
            inner: Arc::new(Mutex::new(HashMap::new())),
        }
    }

    /// Submit a new approval request. Returns a unique ID.
    pub async fn submit(&self, id: String, request: ApprovalRequest) -> String {
        let entry = Entry {
            request,
            resolution: None,
            created: Instant::now(),
            notify: Arc::new(Notify::new()),
        };
        self.inner.lock().await.insert(id.clone(), entry);
        id
    }

    /// Resolve a pending approval with a decision. Returns true if found.
    pub async fn resolve(&self, id: &str, decision: ApprovalDecision) -> bool {
        let mut guard = self.inner.lock().await;
        if let Some(entry) = guard.get_mut(id) {
            entry.resolution = Some(ApprovalResolution::Decision(decision));
            entry.notify.notify_waiters();
            true
        } else {
            false
        }
    }

    pub async fn dismiss_matching(&self, event: &LocalHookEvent) -> Vec<String> {
        let mut guard = self.inner.lock().await;
        let mut dismissed = Vec::new();

        for (id, entry) in guard.iter_mut() {
            if entry.resolution.is_some() || entry.created.elapsed() > APPROVAL_TIMEOUT {
                continue;
            }
            if !matches_event(entry, event) {
                continue;
            }

            entry.resolution = Some(ApprovalResolution::Dismissed);
            entry.notify.notify_waiters();
            dismissed.push(id.clone());
        }

        dismissed
    }

    /// Get the current status of an approval.
    pub async fn status(&self, id: &str) -> Option<ApprovalStatus> {
        let guard = self.inner.lock().await;
        guard.get(id).map(|entry| {
            let timed_out =
                entry.resolution.is_none() && entry.created.elapsed() > APPROVAL_TIMEOUT;
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
                let entry = guard.get(id)?;
                match entry.resolution {
                    Some(ApprovalResolution::Decision(decision)) => return Some(decision),
                    Some(ApprovalResolution::Dismissed) => return None,
                    None => {}
                }
                if entry.created.elapsed() > APPROVAL_TIMEOUT {
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

fn matches_event(entry: &Entry, event: &LocalHookEvent) -> bool {
    if entry.request.session_id.is_empty() || event.session_id.is_empty() {
        return false;
    }
    if entry.request.session_id != event.session_id {
        return false;
    }

    match (
        entry.request.tool_use_id.as_deref(),
        event.tool_use_id.as_deref(),
    ) {
        (Some(expected), Some(actual)) => expected == actual,
        _ => true,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn submit_and_resolve() {
        let store = ApprovalStore::new();
        let id = store
            .submit(
                "test-1".into(),
                ApprovalRequest {
                    tool_name: "Bash".into(),
                    tool_input_summary: "rm -rf /tmp".into(),
                    permission_mode: "default".into(),
                    session_id: "sess-1".into(),
                    tool_use_id: Some("tool-1".into()),
                },
            )
            .await;

        let status = store.status(&id).await.unwrap();
        assert!(!status.resolved);
        assert!(status.decision.is_none());
        assert!(!status.dismissed);

        assert!(store.resolve(&id, ApprovalDecision::Allow).await);

        let status = store.status(&id).await.unwrap();
        assert!(status.resolved);
        assert_eq!(status.decision, Some(ApprovalDecision::Allow));
        assert!(!status.dismissed);
    }

    #[tokio::test]
    async fn wait_resolves_on_notify() {
        let store = ApprovalStore::new();
        let id = store
            .submit(
                "test-2".into(),
                ApprovalRequest {
                    tool_name: "Edit".into(),
                    tool_input_summary: "edit file.rs".into(),
                    permission_mode: "default".into(),
                    session_id: "sess-1".into(),
                    tool_use_id: Some("tool-2".into()),
                },
            )
            .await;

        let store2 = store.clone();
        let id2 = id.clone();
        tokio::spawn(async move {
            tokio::time::sleep(Duration::from_millis(50)).await;
            store2.resolve(&id2, ApprovalDecision::Deny).await;
        });

        let decision = store
            .wait_for_decision(&id, Duration::from_millis(10))
            .await;
        assert_eq!(decision, Some(ApprovalDecision::Deny));
    }

    #[tokio::test]
    async fn dismisses_matching_pending_approvals() {
        let store = ApprovalStore::new();
        let matching = store
            .submit(
                "approve-1".into(),
                ApprovalRequest {
                    tool_name: "Bash".into(),
                    tool_input_summary: "rm -rf /tmp".into(),
                    permission_mode: "default".into(),
                    session_id: "sess-1".into(),
                    tool_use_id: Some("tool-1".into()),
                },
            )
            .await;
        let other = store
            .submit(
                "approve-2".into(),
                ApprovalRequest {
                    tool_name: "Edit".into(),
                    tool_input_summary: "edit file.rs".into(),
                    permission_mode: "default".into(),
                    session_id: "sess-2".into(),
                    tool_use_id: Some("tool-2".into()),
                },
            )
            .await;

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
                recv_ts: 1,
                claude_pid: None,
            })
            .await;

        assert_eq!(dismissed, vec![matching.clone()]);

        let matching_status = store.status(&matching).await.unwrap();
        assert!(matching_status.resolved);
        assert!(matching_status.dismissed);
        assert!(matching_status.decision.is_none());

        let other_status = store.status(&other).await.unwrap();
        assert!(!other_status.resolved);
        assert!(!other_status.dismissed);
    }
}
