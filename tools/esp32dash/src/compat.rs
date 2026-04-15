use std::env;

pub const DEFAULT_ADMIN_ADDR: &str = "127.0.0.1:37125";
pub const DEFAULT_SERIAL_BAUD: u32 = 115_200;

pub fn env_value(name: &str) -> Option<String> {
    env::var(name).ok()
}

pub fn env_value_or(name: &str, default: &str) -> String {
    env_value(name).unwrap_or_else(|| default.to_string())
}

pub fn admin_addr() -> String {
    env_value_or("ESP32DASH_ADMIN_ADDR", DEFAULT_ADMIN_ADDR)
}

pub fn state_dir() -> Option<String> {
    env_value("ESP32DASH_STATE_DIR")
}

pub fn serial_port() -> Option<String> {
    env_value("ESP32DASH_SERIAL_PORT")
}

fn parse_serial_baud_value(raw: &str) -> Option<u32> {
    raw.parse::<u32>().ok()
}

pub fn serial_baud_env() -> Option<u32> {
    let raw = env_value("ESP32DASH_SERIAL_BAUD")?;
    match parse_serial_baud_value(&raw) {
        Some(baud) => Some(baud),
        None => {
            tracing::warn!(
                "invalid ESP32DASH_SERIAL_BAUD={raw:?}, ignoring override and falling back to config/default"
            );
            None
        }
    }
}

pub fn serial_baud() -> u32 {
    serial_baud_env().unwrap_or(DEFAULT_SERIAL_BAUD)
}

#[cfg(test)]
mod tests {
    use super::parse_serial_baud_value;

    #[test]
    fn parse_serial_baud_value_accepts_valid_numbers() {
        assert_eq!(parse_serial_baud_value("230400"), Some(230_400));
    }

    #[test]
    fn parse_serial_baud_value_rejects_invalid_numbers() {
        assert_eq!(parse_serial_baud_value("garbage"), None);
    }
}
