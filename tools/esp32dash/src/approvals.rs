use std::{
    collections::HashMap,
    sync::Arc,
    time::{Duration, Instant},
};

use serde::{Deserialize, Serialize};
use tokio::sync::{Mutex, Notify};

const APPROVAL_TIMEOUT: Duration = Duration::from_secs(300);
const CLEANUP_THRESHOLD: usize = 32;

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
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ApprovalStatus {
    pub id: String,
    pub tool_name: String,
    pub tool_input_summary: String,
    pub decision: Option<ApprovalDecision>,
    pub resolved: bool,
    pub timed_out: bool,
}

#[derive(Debug)]
struct Entry {
    request: ApprovalRequest,
    decision: Option<ApprovalDecision>,
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
            decision: None,
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
            entry.decision = Some(decision);
            entry.notify.notify_waiters();
            true
        } else {
            false
        }
    }

    /// Get the current status of an approval.
    pub async fn status(&self, id: &str) -> Option<ApprovalStatus> {
        let guard = self.inner.lock().await;
        guard.get(id).map(|entry| {
            let timed_out = entry.decision.is_none() && entry.created.elapsed() > APPROVAL_TIMEOUT;
            ApprovalStatus {
                id: id.to_string(),
                tool_name: entry.request.tool_name.clone(),
                tool_input_summary: entry.request.tool_input_summary.clone(),
                decision: entry.decision,
                resolved: entry.decision.is_some() || timed_out,
                timed_out,
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
                if entry.decision.is_some() {
                    return entry.decision;
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

    /// Get the oldest pending approval (for device dispatch).
    pub async fn oldest_pending(&self) -> Option<(String, ApprovalRequest)> {
        let guard = self.inner.lock().await;
        guard
            .iter()
            .filter(|(_, entry)| entry.decision.is_none() && entry.created.elapsed() < APPROVAL_TIMEOUT)
            .min_by_key(|(_, entry)| entry.created)
            .map(|(id, entry)| (id.clone(), entry.request.clone()))
    }

    /// Clean up expired entries.
    pub async fn cleanup(&self) {
        let mut guard = self.inner.lock().await;
        if guard.len() < CLEANUP_THRESHOLD {
            return;
        }
        guard.retain(|_, entry| {
            entry.created.elapsed() < APPROVAL_TIMEOUT * 2
        });
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
                },
            )
            .await;

        let status = store.status(&id).await.unwrap();
        assert!(!status.resolved);
        assert!(status.decision.is_none());

        assert!(store.resolve(&id, ApprovalDecision::Allow).await);

        let status = store.status(&id).await.unwrap();
        assert!(status.resolved);
        assert_eq!(status.decision, Some(ApprovalDecision::Allow));
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
}
