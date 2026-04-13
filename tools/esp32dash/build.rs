use std::{env, fs, path::PathBuf};

fn main() {
    let manifest_dir = PathBuf::from(env::var("CARGO_MANIFEST_DIR").expect("CARGO_MANIFEST_DIR"));
    let font_source = manifest_dir.join("../../src/apps/app_home/src/generated/noto_sans_cjk_12.c");

    println!("cargo:rerun-if-changed={}", font_source.display());

    let source = fs::read_to_string(&font_source)
        .unwrap_or_else(|err| panic!("failed to read {}: {err}", font_source.display()));
    let symbols = extract_symbols(&source)
        .unwrap_or_else(|err| panic!("failed to extract CJK font symbols: {err}"));
    let escaped: String = symbols.chars().flat_map(|ch| ch.escape_default()).collect();

    let out_path =
        PathBuf::from(env::var("OUT_DIR").expect("OUT_DIR")).join("supported_cjk_chars.rs");
    fs::write(out_path, format!("pub const FONT_SUPPORTED_NON_ASCII: &str = \"{escaped}\";\n"))
        .expect("failed to write generated supported_cjk_chars.rs");
}

fn extract_symbols(source: &str) -> Result<String, &'static str> {
    let marker = "--symbols";
    let start = source.find(marker).ok_or("missing --symbols marker")?;
    let tail = &source[start + marker.len()..];
    let end = tail.find("\n * --size").ok_or("missing --size marker")?;

    let mut symbols = String::new();
    for line in tail[..end].lines() {
        let trimmed = line.trim_start();
        if let Some(content) = trimmed.strip_prefix('*') {
            let content = content.trim_start();
            if !content.is_empty() {
                symbols.push_str(content);
            }
        }
    }

    if symbols.is_empty() {
        return Err("empty symbol block");
    }

    Ok(symbols)
}
