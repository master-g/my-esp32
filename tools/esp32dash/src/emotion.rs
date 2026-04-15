use std::net::{IpAddr, SocketAddr, ToSocketAddrs};

use anyhow::{Context, Result, anyhow};
use reqwest::{Client, Url};
use serde::Deserialize;
use serde_json::{Value, json};
use tracing::warn;

use crate::config::EmotionConfig;

const ANTHROPIC_VERSION: &str = "2023-06-01";
const DEFAULT_SYSTEM_PROMPT: &str = "Classify the user's emotional tone. Respond with JSON only in the shape {\"emotion\":\"neutral|happy|sad|sob\",\"confidence\":0.0}. Use sob only for obvious intense distress. Do not add markdown or explanation.";
const MIN_EMOTION_MAX_TOKENS: u32 = 256;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Emotion {
    Neutral,
    Happy,
    Sad,
    Sob,
}

impl Emotion {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Neutral => "neutral",
            Self::Happy => "happy",
            Self::Sad => "sad",
            Self::Sob => "sob",
        }
    }

    pub fn from_str(value: &str) -> Self {
        match value.trim().to_ascii_lowercase().as_str() {
            "happy" => Self::Happy,
            "sad" => Self::Sad,
            "sob" => Self::Sob,
            _ => Self::Neutral,
        }
    }
}

#[derive(Debug, Clone, PartialEq)]
pub struct EmotionAnalysis {
    pub emotion: Emotion,
    pub confidence: f32,
}

impl EmotionAnalysis {
    pub fn neutral() -> Self {
        Self {
            emotion: Emotion::Neutral,
            confidence: 0.0,
        }
    }
}

#[derive(Debug, Clone)]
pub struct EmotionAnalyzer {
    endpoint: String,
    model: String,
    api_key: String,
    timeout_secs: u64,
    max_tokens: u32,
}

impl EmotionAnalyzer {
    pub fn from_config(config: &EmotionConfig) -> Option<Self> {
        if !config.enabled || !config.has_provider_config() {
            return None;
        }

        let endpoint =
            match validate_messages_endpoint(config.api_base_url.as_deref().unwrap_or_default()) {
                Ok(endpoint) => endpoint,
                Err(err) => {
                    warn!(error = %err, "invalid emotion api_base_url, disabling emotion analyzer");
                    return None;
                }
            };

        Some(Self {
            endpoint,
            model: config.model.clone().unwrap_or_default(),
            api_key: config.api_key.clone().unwrap_or_default(),
            timeout_secs: config.timeout_secs,
            max_tokens: effective_max_tokens(config.max_tokens),
        })
    }

    pub async fn analyze(&self, prompt_raw: &str) -> Result<EmotionAnalysis> {
        let client = build_client_for_endpoint(&self.endpoint)?;
        let payload = json!({
            "model": self.model,
            "max_tokens": self.max_tokens,
            "temperature": 0.1,
            "system": DEFAULT_SYSTEM_PROMPT,
            "messages": [{
                "role": "user",
                "content": [{
                    "type": "text",
                    "text": prompt_raw,
                }],
            }],
        });

        let response = client
            .post(&self.endpoint)
            .timeout(std::time::Duration::from_secs(self.timeout_secs))
            .header("content-type", "application/json")
            .header("accept", "application/json")
            .header("x-api-key", &self.api_key)
            .header("anthropic-version", ANTHROPIC_VERSION)
            .json(&payload)
            .send()
            .await
            .with_context(|| format!("emotion request failed for {}", self.endpoint))?;

        let status = response.status();
        if !status.is_success() {
            return Err(anyhow!("emotion request returned {}", status));
        }

        let body: Value =
            response.json().await.context("failed to decode emotion response JSON")?;
        let text = first_text_block(&body).ok_or_else(|| missing_text_block_error(&body))?;
        parse_analysis_text(&text)
    }
}

fn effective_max_tokens(configured: u32) -> u32 {
    configured.max(MIN_EMOTION_MAX_TOKENS)
}

fn canonicalize_messages_endpoint(base_url: &str) -> String {
    let trimmed = base_url.trim().trim_end_matches('/');
    if trimmed.ends_with("/v1/messages") {
        trimmed.to_string()
    } else {
        format!("{trimmed}/v1/messages")
    }
}

fn validate_messages_endpoint(base_url: &str) -> Result<String> {
    let endpoint = canonicalize_messages_endpoint(base_url);
    let url = Url::parse(&endpoint)
        .with_context(|| format!("invalid emotion api endpoint {endpoint}"))?;
    let host = url.host_str().ok_or_else(|| anyhow!("emotion api endpoint must include host"))?;
    if url.scheme() != "https" {
        return Err(anyhow!("emotion api endpoint must use https"));
    }
    if is_local_or_private_host(host) {
        return Err(anyhow!(
            "emotion api endpoint host must not be localhost or a private IP literal"
        ));
    }
    Ok(endpoint)
}

fn build_client_for_endpoint(endpoint: &str) -> Result<Client> {
    let url =
        Url::parse(endpoint).with_context(|| format!("invalid emotion api endpoint {endpoint}"))?;
    let host = url.host_str().ok_or_else(|| anyhow!("emotion api endpoint must include host"))?;
    if is_local_or_private_host(host) {
        return Err(anyhow!(
            "emotion api endpoint host must not be localhost or a private IP literal"
        ));
    }
    if host.parse::<IpAddr>().is_ok() {
        return Ok(Client::new());
    }

    let port = url
        .port_or_known_default()
        .ok_or_else(|| anyhow!("emotion api endpoint must specify a known port"))?;
    let addrs: Vec<SocketAddr> = (host, port)
        .to_socket_addrs()
        .with_context(|| format!("failed to resolve emotion api host {host}"))?
        .collect();
    if addrs.is_empty() {
        return Err(anyhow!("emotion api host resolved to no addresses"));
    }
    if addrs.iter().any(|addr| is_disallowed_ip(addr.ip())) {
        return Err(anyhow!("emotion api host resolved to a private or local address"));
    }

    Client::builder()
        .resolve_to_addrs(host, &addrs)
        .build()
        .context("failed to build pinned emotion http client")
}

fn is_local_or_private_host(host: &str) -> bool {
    if host.eq_ignore_ascii_case("localhost") || host.ends_with(".localhost") {
        return true;
    }
    match host.parse::<IpAddr>() {
        Ok(ip) => is_disallowed_ip(ip),
        Err(_) => false,
    }
}

fn is_disallowed_ip(ip: IpAddr) -> bool {
    match ip {
        IpAddr::V4(ip) => {
            ip.is_private()
                || ip.is_loopback()
                || ip.is_link_local()
                || ip.is_multicast()
                || ip.is_unspecified()
        }
        IpAddr::V6(ip) => {
            ip.is_loopback()
                || ip.is_unspecified()
                || ip.is_multicast()
                || ip.is_unique_local()
                || ip.is_unicast_link_local()
        }
    }
}

fn first_text_block(body: &Value) -> Option<String> {
    body.get("content")?.as_array()?.iter().find_map(|block| {
        let text = block.get("text")?.as_str()?.trim();
        let block_type = block.get("type").and_then(Value::as_str).unwrap_or_default();
        if text.is_empty() || (!block_type.is_empty() && block_type != "text") {
            return None;
        }
        Some(text.to_string())
    })
}

fn missing_text_block_error(body: &Value) -> anyhow::Error {
    let stop_reason = body.get("stop_reason").and_then(Value::as_str).unwrap_or_default();
    let saw_thinking = body.get("content").and_then(Value::as_array).is_some_and(|blocks| {
        blocks.iter().any(|block| block.get("type").and_then(Value::as_str) == Some("thinking"))
    });
    if saw_thinking && stop_reason == "max_tokens" {
        anyhow!("emotion response exhausted max_tokens before emitting text block")
    } else {
        anyhow!("emotion response missing text block")
    }
}

fn parse_analysis_text(text: &str) -> Result<EmotionAnalysis> {
    let stripped = strip_code_fences(text);
    let candidate = extract_json_object(&stripped).unwrap_or(&stripped);
    let payload: EmotionPayload =
        serde_json::from_str(candidate).context("failed to parse emotion JSON payload")?;
    Ok(EmotionAnalysis {
        emotion: Emotion::from_str(&payload.emotion),
        confidence: payload.confidence.unwrap_or(0.0).clamp(0.0, 1.0),
    })
}

fn strip_code_fences(text: &str) -> String {
    let trimmed = text.trim();
    if !trimmed.starts_with("```") {
        return trimmed.to_string();
    }

    let without_start = match trimmed.find('\n') {
        Some(index) => &trimmed[index + 1..],
        None => return trimmed.to_string(),
    };
    without_start.trim_end().strip_suffix("```").unwrap_or(without_start).trim().to_string()
}

fn extract_json_object(text: &str) -> Option<&str> {
    let start = text.find('{')?;
    let end = text.rfind('}')?;
    (start < end).then_some(&text[start..=end])
}

#[derive(Debug, Deserialize)]
struct EmotionPayload {
    emotion: String,
    #[serde(default)]
    confidence: Option<f32>,
}

#[cfg(test)]
mod tests {
    use serde_json::json;
    use std::net::{IpAddr, Ipv4Addr};

    use super::{
        Emotion, build_client_for_endpoint, canonicalize_messages_endpoint, effective_max_tokens,
        first_text_block, is_disallowed_ip, missing_text_block_error, parse_analysis_text,
        strip_code_fences, validate_messages_endpoint,
    };

    #[test]
    fn canonicalize_endpoint_appends_messages_path() {
        assert_eq!(
            canonicalize_messages_endpoint("https://api.minimaxi.com/anthropic"),
            "https://api.minimaxi.com/anthropic/v1/messages"
        );
    }

    #[test]
    fn canonicalize_endpoint_preserves_messages_path() {
        assert_eq!(
            canonicalize_messages_endpoint("https://api.minimaxi.com/anthropic/v1/messages"),
            "https://api.minimaxi.com/anthropic/v1/messages"
        );
    }

    #[test]
    fn validate_endpoint_accepts_public_https_url() {
        assert_eq!(
            validate_messages_endpoint("https://api.minimaxi.com/anthropic").unwrap(),
            "https://api.minimaxi.com/anthropic/v1/messages"
        );
    }

    #[test]
    fn validate_endpoint_rejects_http() {
        assert_eq!(
            validate_messages_endpoint("http://api.minimaxi.com/anthropic")
                .expect_err("http endpoint should be rejected")
                .to_string(),
            "emotion api endpoint must use https"
        );
    }

    #[test]
    fn validate_endpoint_rejects_localhost() {
        assert_eq!(
            validate_messages_endpoint("https://localhost:8080/anthropic")
                .expect_err("localhost endpoint should be rejected")
                .to_string(),
            "emotion api endpoint host must not be localhost or a private IP literal"
        );
    }

    #[test]
    fn validate_endpoint_rejects_private_ip_literal() {
        assert_eq!(
            validate_messages_endpoint("https://10.0.0.5/anthropic")
                .expect_err("private ip should be rejected")
                .to_string(),
            "emotion api endpoint host must not be localhost or a private IP literal"
        );
    }

    #[test]
    fn build_client_for_endpoint_rejects_dns_results_on_private_ip() {
        assert_eq!(
            build_client_for_endpoint("https://localhost:8443/v1/messages")
                .expect_err("localhost should be rejected before request time")
                .to_string(),
            "emotion api endpoint host must not be localhost or a private IP literal"
        );
    }

    #[test]
    fn disallowed_ip_rejects_private_ipv4() {
        assert!(is_disallowed_ip(IpAddr::V4(Ipv4Addr::new(10, 0, 0, 5))));
        assert!(!is_disallowed_ip(IpAddr::V4(Ipv4Addr::new(1, 1, 1, 1))));
    }

    #[test]
    fn first_text_block_skips_thinking_blocks() {
        let body = json!({
            "content": [
                {"type": "thinking", "thinking": "working"},
                {"type": "text", "text": "{\"emotion\":\"happy\",\"confidence\":0.7}"}
            ]
        });

        assert_eq!(
            first_text_block(&body).as_deref(),
            Some("{\"emotion\":\"happy\",\"confidence\":0.7}")
        );
    }

    #[test]
    fn effective_max_tokens_enforces_reasoning_floor() {
        assert_eq!(effective_max_tokens(64), 256);
        assert_eq!(effective_max_tokens(512), 512);
    }

    #[test]
    fn missing_text_block_error_mentions_token_exhaustion() {
        let body = json!({
            "content": [
                {"type": "thinking", "thinking": "working"}
            ],
            "stop_reason": "max_tokens"
        });

        assert_eq!(
            missing_text_block_error(&body).to_string(),
            "emotion response exhausted max_tokens before emitting text block"
        );
    }

    #[test]
    fn strip_code_fences_removes_wrapping_markdown() {
        assert_eq!(
            strip_code_fences("```json\n{\"emotion\":\"sad\"}\n```"),
            "{\"emotion\":\"sad\"}"
        );
    }

    #[test]
    fn parse_analysis_text_understands_fenced_json() {
        let analysis =
            parse_analysis_text("```json\n{\"emotion\":\"sob\",\"confidence\":1.2}\n```")
                .expect("fenced json should parse");
        assert_eq!(analysis.emotion, Emotion::Sob);
        assert_eq!(analysis.confidence, 1.0);
    }
}
