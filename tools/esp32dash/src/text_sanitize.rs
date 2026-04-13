use std::{collections::HashSet, path::Path, sync::OnceLock};

include!(concat!(env!("OUT_DIR"), "/supported_cjk_chars.rs"));

pub const WORKSPACE_MAX_BYTES: usize = 24;
pub const TITLE_MAX_BYTES: usize = 40;
pub const DETAIL_MAX_BYTES: usize = 96;
pub const PROMPT_MAX_BYTES: usize = 96;
pub const TOOL_NAME_MAX_BYTES: usize = 48;
pub const APPROVAL_DESC_MAX_BYTES: usize = 80;
pub const APPROVAL_ID_MAX_BYTES: usize = 31;
pub const MESSAGE_MAX_BYTES: usize = 96;
pub const PERMISSION_MODE_MAX_BYTES: usize = 16;

static SUPPORTED_NON_ASCII_SET: OnceLock<HashSet<char>> = OnceLock::new();

fn supported_non_ascii_set() -> &'static HashSet<char> {
    SUPPORTED_NON_ASCII_SET.get_or_init(|| FONT_SUPPORTED_NON_ASCII.chars().collect())
}

pub fn sanitize_display_text(input: &str, max_bytes: usize) -> String {
    if max_bytes == 0 {
        return String::new();
    }

    let mut out = String::new();
    let mut prev_space = true;

    for ch in first_line(input).chars() {
        let mapped = map_display_char(ch);
        if mapped == ' ' {
            if prev_space {
                continue;
            }
        }

        if !push_if_fits(&mut out, mapped, max_bytes) {
            break;
        }
        prev_space = mapped == ' ';
    }

    if out.ends_with(' ') {
        out.pop();
    }

    out
}

pub fn sanitize_prompt_preview(input: &str) -> Option<String> {
    let preview = sanitize_display_text(input, PROMPT_MAX_BYTES);
    (!preview.is_empty()).then_some(preview)
}

pub fn sanitize_message(input: &str) -> Option<String> {
    let message = sanitize_display_text(input, MESSAGE_MAX_BYTES);
    (!message.is_empty()).then_some(message)
}

pub fn sanitize_tool_name(input: &str) -> Option<String> {
    let tool_name = sanitize_display_text(input, TOOL_NAME_MAX_BYTES);
    (!tool_name.is_empty()).then_some(tool_name)
}

pub fn sanitize_permission_mode(input: &str) -> String {
    sanitize_display_text(input, PERMISSION_MODE_MAX_BYTES)
}

pub fn sanitize_snapshot_title(input: &str) -> String {
    sanitize_display_text(input, TITLE_MAX_BYTES)
}

pub fn sanitize_snapshot_detail(input: &str) -> String {
    sanitize_display_text(input, DETAIL_MAX_BYTES)
}

pub fn sanitize_approval_summary(input: &str) -> String {
    sanitize_display_text(input, APPROVAL_DESC_MAX_BYTES)
}

pub fn build_workspace(cwd: &str) -> String {
    Path::new(cwd)
        .file_name()
        .and_then(|name| name.to_str())
        .map(|name| sanitize_display_text(name, WORKSPACE_MAX_BYTES))
        .unwrap_or_default()
}

pub fn build_approval_id(now_epoch: u64, tool_name: &str) -> String {
    truncate_ascii_token(
        &format!("approval-{now_epoch:x}-{:08x}", fnv1a_32(tool_name.as_bytes())),
        APPROVAL_ID_MAX_BYTES,
    )
}

pub fn sanitize_transport_id(input: &str) -> String {
    let token = truncate_ascii_token(input, APPROVAL_ID_MAX_BYTES);
    if token.is_empty() {
        return format!("approval-{:08x}", fnv1a_32(input.as_bytes()));
    }
    token
}

fn first_line(input: &str) -> &str {
    input.lines().next().unwrap_or_default().trim()
}

fn map_display_char(ch: char) -> char {
    if ch.is_ascii_control() || ch.is_whitespace() {
        return ' ';
    }
    if ch.is_ascii() || supported_non_ascii_set().contains(&ch) {
        return ch;
    }
    '?'
}

fn push_if_fits(out: &mut String, ch: char, max_bytes: usize) -> bool {
    if out.len() + ch.len_utf8() > max_bytes {
        return false;
    }
    out.push(ch);
    true
}

fn truncate_ascii_token(input: &str, max_bytes: usize) -> String {
    if max_bytes == 0 {
        return String::new();
    }

    let mut out = String::new();
    let mut prev_sep = true;

    for ch in input.chars() {
        let mapped = match ch {
            'a'..='z' | '0'..='9' => ch,
            'A'..='Z' => ch.to_ascii_lowercase(),
            '-' | '_' => '-',
            _ => '-',
        };

        if mapped == '-' {
            if prev_sep || !push_if_fits(&mut out, '-', max_bytes) {
                continue;
            }
            prev_sep = true;
            continue;
        }

        if !push_if_fits(&mut out, mapped, max_bytes) {
            break;
        }
        prev_sep = false;
    }

    while out.ends_with('-') {
        out.pop();
    }

    out
}

fn fnv1a_32(bytes: &[u8]) -> u32 {
    let mut hash = 0x811c9dc5u32;
    for byte in bytes {
        hash ^= u32::from(*byte);
        hash = hash.wrapping_mul(0x01000193);
    }
    hash
}

#[cfg(test)]
mod tests {
    use super::{
        build_approval_id, build_workspace, sanitize_approval_summary, sanitize_display_text,
        sanitize_tool_name, sanitize_transport_id,
    };

    #[test]
    fn supported_cjk_survives() {
        assert_eq!(sanitize_display_text("你好，世界", 96), "你好，世界");
    }

    #[test]
    fn emoji_is_replaced() {
        assert_eq!(sanitize_display_text("building 🔨 stuff", 96), "building ? stuff");
    }

    #[test]
    fn utf8_truncation_stays_on_char_boundary() {
        assert_eq!(sanitize_display_text("你好世界", 5), "你");
    }

    #[test]
    fn approval_summary_uses_device_limit() {
        assert_eq!(sanitize_approval_summary("创建文件 test.rs 并写入"), "创建文件 test.rs 并写入");
    }

    #[test]
    fn workspace_uses_last_path_component() {
        assert_eq!(build_workspace("/tmp/project-foo"), "project-foo");
    }

    #[test]
    fn tool_name_is_byte_safe() {
        assert_eq!(sanitize_tool_name("exec_command").as_deref(), Some("exec_command"));
    }

    #[test]
    fn approval_id_is_ascii_and_bounded() {
        let approval_id = build_approval_id(1_775_742_746, "创建文件");
        assert!(approval_id.is_ascii());
        assert!(approval_id.len() <= 31);
    }

    #[test]
    fn transport_id_falls_back_to_hashed_ascii() {
        let transport_id = sanitize_transport_id("审批🔥");
        assert!(transport_id.is_ascii());
        assert!(!transport_id.is_empty());
        assert!(transport_id.len() <= 31);
    }
}
